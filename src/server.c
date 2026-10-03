/*
 * server.c - the core of the server.
 *
 * Architecture (one acceptor/event thread + N workers):
 *
 *   event loop thread                     worker threads (pthreads)
 *   -----------------                     -------------------------
 *   kqueue/epoll wait  ── fd readable ──▶ queue ──▶ recv into conn buffer
 *   accept() new conns                              parse request(s)
 *   reap idle conns                                 route + write response
 *   watch shutdown pipe  ◀──── re-arm fd ────────── (or close connection)
 *
 * Every client fd is registered one-shot, so once the loop hands a connection
 * to a worker it will not fire again until that worker re-arms it. That gives
 * each connection a single owner without per-connection locks. The connection
 * table itself is guarded by one mutex because accept, close and the idle
 * reaper all mutate it from different threads.
 */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "event.h"
#include "http.h"
#include "router.h"
#include "threadpool.h"

#define LISTEN_BACKLOG 1024
#define QUEUE_CAPACITY 8192
#define MAX_FDS 65536

typedef enum { CONN_ARMED, CONN_BUSY } conn_state;

typedef struct {
    int fd;
    conn_state state;
    time_t last_active;
    size_t len;
    char buf[MAX_REQUEST_SIZE];
} conn;

static struct {
    server_config cfg;
    int listen_fd;
    int wake_pipe[2];
    ev_loop *loop;
    thread_pool *pool;
    time_t started;

    pthread_mutex_t table_lock;
    conn **conns; /* indexed by fd */
    int max_fds;
    int high_fd;
} S = {.listen_fd = -1, .wake_pipe = {-1, -1}};

static atomic_int g_stop;
static atomic_ullong g_requests;
static atomic_ullong g_active;

/* ---------- small helpers ---------- */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }

void server_request_shutdown(void) {
    int saved = errno;
    atomic_store(&g_stop, 1);
    if (S.wake_pipe[1] >= 0) {
        ssize_t r = write(S.wake_pipe[1], "x", 1);
        (void)r;
    }
    errno = saved;
}

void server_get_stats(server_stats *out) {
    out->requests_total = atomic_load(&g_requests);
    out->active_connections = atomic_load(&g_active);
    out->worker_threads = S.cfg.threads;
    out->backend = ev_backend_name();
    out->uptime_seconds = (long)(time(NULL) - S.started);
}

/* ---------- connection lifecycle ---------- */

/* Caller must hold table_lock. Closing under the lock prevents the fd number
 * from being reused by accept() before its table slot is cleared. */
static void close_conn_locked(conn *c) {
    S.conns[c->fd] = NULL;
    close(c->fd); /* also removes it from kqueue/epoll */
    free(c);
    atomic_fetch_sub(&g_active, 1);
}

static void close_conn(conn *c) {
    pthread_mutex_lock(&S.table_lock);
    close_conn_locked(c);
    pthread_mutex_unlock(&S.table_lock);
}

/* After an error response the client may still be uploading a body we refused.
 * Closing with unread data makes the kernel send RST, which can destroy the
 * response before the client reads it, so half-close and drain briefly first. */
static void linger_close(conn *c) {
    shutdown(c->fd, SHUT_WR);
    char sink[4096];
    for (int waited = 0; waited < 500; waited += 50) {
        struct pollfd pfd = {.fd = c->fd, .events = POLLIN};
        if (poll(&pfd, 1, 50) < 0) break;
        ssize_t n = recv(c->fd, sink, sizeof sink, 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
    }
    close_conn(c);
}

static void rearm_conn(conn *c) {
    pthread_mutex_lock(&S.table_lock);
    c->last_active = time(NULL);
    c->state = CONN_ARMED;
    if (ev_rearm(S.loop, c->fd) < 0) close_conn_locked(c);
    pthread_mutex_unlock(&S.table_lock);
}

/* ---------- worker side ---------- */

static void handle_conn(void *arg) {
    conn *c = arg;

    /* Drain everything the kernel has for us (non-blocking socket). */
    while (c->len < sizeof c->buf) {
        ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
        if (n > 0) {
            c->len += (size_t)n;
        } else if (n == 0) {
            close_conn(c); /* peer closed */
            return;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            close_conn(c);
            return;
        }
    }

    /* Serve every complete request in the buffer (supports pipelining). */
    while (c->len > 0) {
        http_request req;
        parse_result r = http_parse_request(c->buf, c->len, &req);
        if (r == PARSE_INCOMPLETE) break;
        if (r != PARSE_OK) {
            send_error(c->fd, http_status_for_parse_result(r), 0);
            linger_close(c);
            return;
        }

        int keep_alive = req.keep_alive && !atomic_load(&g_stop);
        int rc = router_handle(c->fd, &req, keep_alive);
        atomic_fetch_add(&g_requests, 1);
        if (S.cfg.verbose)
            fprintf(stderr, "[fd %d] %s %s%s%s\n", c->fd, req.method, req.path,
                    req.query[0] ? "?" : "", req.query);

        size_t used = req.header_len + req.content_length;
        memmove(c->buf, c->buf + used, c->len - used);
        c->len -= used;

        if (rc < 0 || !keep_alive) {
            close_conn(c);
            return;
        }
    }

    rearm_conn(c);
}

/* ---------- event loop side ---------- */

static void accept_connections(void) {
    for (;;) {
        int fd = accept(S.listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                fprintf(stderr, "accept: %s\n", strerror(errno));
            return;
        }
        if (fd >= S.max_fds || set_nonblocking(fd) < 0) {
            close(fd);
            continue;
        }
        set_cloexec(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

        conn *c = malloc(sizeof *c);
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->len = 0;
        c->state = CONN_ARMED;
        c->last_active = time(NULL);

        pthread_mutex_lock(&S.table_lock);
        S.conns[fd] = c;
        if (fd > S.high_fd) S.high_fd = fd;
        atomic_fetch_add(&g_active, 1);
        if (ev_add(S.loop, fd, 1) < 0) close_conn_locked(c);
        pthread_mutex_unlock(&S.table_lock);
    }
}

static void dispatch(int fd) {
    pthread_mutex_lock(&S.table_lock);
    conn *c = (fd >= 0 && fd < S.max_fds) ? S.conns[fd] : NULL;
    if (c && c->state == CONN_ARMED)
        c->state = CONN_BUSY;
    else
        c = NULL;
    pthread_mutex_unlock(&S.table_lock);
    if (!c) return;

    if (threadpool_submit(S.pool, handle_conn, c) < 0) {
        /* Backpressure: queue is full, shed load instead of stalling the loop. */
        send_error(c->fd, 503, 0);
        close_conn(c);
    }
}

static void reap_idle(time_t now) {
    pthread_mutex_lock(&S.table_lock);
    for (int fd = 0; fd <= S.high_fd; fd++) {
        conn *c = S.conns[fd];
        if (c && c->state == CONN_ARMED && now - c->last_active >= S.cfg.idle_timeout_sec)
            close_conn_locked(c);
    }
    pthread_mutex_unlock(&S.table_lock);
}

static int open_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, LISTEN_BACKLOG) < 0 ||
        set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }
    set_cloexec(fd);
    return fd;
}

static int compute_max_fds(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < MAX_FDS)
        return (int)rl.rlim_cur;
    return MAX_FDS;
}

int server_run(const server_config *cfg) {
    S.cfg = *cfg;
    S.started = time(NULL);
    S.max_fds = compute_max_fds();
    pthread_mutex_init(&S.table_lock, NULL);

    char docroot[PATH_MAX];
    if (!realpath(cfg->docroot, docroot)) {
        fprintf(stderr, "docroot %s: %s\n", cfg->docroot, strerror(errno));
        return 1;
    }
    router_init(docroot);

    S.conns = calloc((size_t)S.max_fds, sizeof *S.conns);
    if (!S.conns || pipe(S.wake_pipe) < 0) {
        perror("setup");
        return 1;
    }
    set_nonblocking(S.wake_pipe[0]);
    set_nonblocking(S.wake_pipe[1]);
    set_cloexec(S.wake_pipe[0]);
    set_cloexec(S.wake_pipe[1]);

    S.listen_fd = open_listener(cfg->port);
    if (S.listen_fd < 0) {
        fprintf(stderr, "listen on port %d: %s\n", cfg->port, strerror(errno));
        return 1;
    }

    /* Workers inherit this mask, so SIGINT/SIGTERM are only ever delivered to
     * the event-loop thread and never interrupt a worker mid-request. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    S.pool = threadpool_create(cfg->threads, QUEUE_CAPACITY);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    S.loop = ev_create();
    if (!S.loop || !S.pool || ev_add(S.loop, S.listen_fd, 0) < 0 ||
        ev_add(S.loop, S.wake_pipe[0], 0) < 0) {
        perror("event loop / thread pool");
        return 1;
    }

    printf("mthttp listening on http://0.0.0.0:%d  (backend=%s, workers=%d, docroot=%s)\n",
           cfg->port, ev_backend_name(), cfg->threads, docroot);
    fflush(stdout);

    int ready[256];
    time_t last_reap = time(NULL);
    while (!atomic_load(&g_stop)) {
        int n = ev_wait(S.loop, ready, 256, 1000);
        if (n < 0) {
            perror("ev_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            if (ready[i] == S.listen_fd)
                accept_connections();
            else if (ready[i] == S.wake_pipe[0])
                atomic_store(&g_stop, 1);
            else
                dispatch(ready[i]);
        }
        time_t now = time(NULL);
        if (now != last_reap) {
            reap_idle(now);
            last_reap = now;
        }
    }

    /* Graceful shutdown: stop accepting, let in-flight requests finish, then
     * close whatever keep-alive connections are left. */
    printf("shutting down: draining %llu connection(s)...\n", atomic_load(&g_active));
    fflush(stdout);
    close(S.listen_fd);
    threadpool_destroy(S.pool);

    pthread_mutex_lock(&S.table_lock);
    for (int fd = 0; fd <= S.high_fd; fd++)
        if (S.conns[fd]) close_conn_locked(S.conns[fd]);
    pthread_mutex_unlock(&S.table_lock);

    ev_destroy(S.loop);
    close(S.wake_pipe[0]);
    close(S.wake_pipe[1]);
    S.wake_pipe[1] = -1;
    free(S.conns);
    pthread_mutex_destroy(&S.table_lock);

    printf("shutdown complete (%llu requests served)\n", atomic_load(&g_requests));
    return 0;
}
