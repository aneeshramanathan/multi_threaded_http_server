#define _GNU_SOURCE /* strcasestr */
/*
 * loadgen.c - minimal keep-alive HTTP load generator.
 *
 * Opens C concurrent connections (one pthread each), fires GET requests
 * back-to-back for D seconds, and reports throughput, latency and failures.
 * Any non-200 response, short read or socket error counts as a failure.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_t tid;
    unsigned long ok, failed;
    double latency_sum_ms, latency_max_ms;
} worker;

static const char *g_host = "127.0.0.1";
static int g_port = 8080;
static const char *g_path = "/";
static double g_deadline;
static char g_request[1024];
static size_t g_request_len;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int connect_once(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)g_port);
    if (inet_pton(AF_INET, g_host, &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

/* Sends one request and reads exactly one response. Returns HTTP status or -1. */
static int do_request(int fd) {
    if (send(fd, g_request, g_request_len, 0) != (ssize_t)g_request_len) return -1;

    char buf[65536];
    size_t len = 0;
    char *hdr_end = NULL;
    while (!hdr_end) {
        ssize_t n = recv(fd, buf + len, sizeof buf - 1 - len, 0);
        if (n <= 0) return -1;
        len += (size_t)n;
        buf[len] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
        if (!hdr_end && len >= sizeof buf - 1) return -1;
    }

    int status = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) return -1;
    char *cl = strcasestr(buf, "\r\nContent-Length:");
    size_t body_len = cl ? strtoul(cl + 17, NULL, 10) : 0;

    size_t have = len - (size_t)(hdr_end + 4 - buf);
    while (have < body_len) {
        size_t want = body_len - have < sizeof buf ? body_len - have : sizeof buf;
        ssize_t n = recv(fd, buf, want, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    return status;
}

static void *run(void *arg) {
    worker *w = arg;
    int fd = -1;
    while (now_sec() < g_deadline) {
        if (fd < 0 && (fd = connect_once()) < 0) {
            w->failed++;
            continue;
        }
        double t0 = now_sec();
        int status = do_request(fd);
        double ms = (now_sec() - t0) * 1000.0;
        if (status == 200) {
            w->ok++;
            w->latency_sum_ms += ms;
            if (ms > w->latency_max_ms) w->latency_max_ms = ms;
        } else {
            w->failed++;
            close(fd);
            fd = -1;
        }
    }
    if (fd >= 0) close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int conns = 100;
    double duration = 10;
    int opt;
    while ((opt = getopt(argc, argv, "h:p:c:d:u:")) != -1) {
        switch (opt) {
        case 'h': g_host = optarg; break;
        case 'p': g_port = atoi(optarg); break;
        case 'c': conns = atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'u': g_path = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-h host] [-p port] [-c conns] [-d seconds] [-u path]\n", argv[0]);
            return 2;
        }
    }

    g_request_len = (size_t)snprintf(g_request, sizeof g_request,
                                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: keep-alive\r\n\r\n",
                                     g_path, g_host, g_port);

    worker *ws = calloc((size_t)conns, sizeof *ws);
    double start = now_sec();
    g_deadline = start + duration;
    for (int i = 0; i < conns; i++) pthread_create(&ws[i].tid, NULL, run, &ws[i]);

    unsigned long ok = 0, failed = 0;
    double lat_sum = 0, lat_max = 0;
    for (int i = 0; i < conns; i++) {
        pthread_join(ws[i].tid, NULL);
        ok += ws[i].ok;
        failed += ws[i].failed;
        lat_sum += ws[i].latency_sum_ms;
        if (ws[i].latency_max_ms > lat_max) lat_max = ws[i].latency_max_ms;
    }
    double elapsed = now_sec() - start;

    printf("connections:   %d\n", conns);
    printf("duration:      %.2f s\n", elapsed);
    printf("requests ok:   %lu\n", ok);
    printf("failures:      %lu\n", failed);
    printf("throughput:    %.0f req/s\n", (double)ok / elapsed);
    printf("latency avg:   %.3f ms\n", ok ? lat_sum / (double)ok : 0.0);
    printf("latency max:   %.3f ms\n", lat_max);
    free(ws);
    return failed == 0 ? 0 : 1;
}
