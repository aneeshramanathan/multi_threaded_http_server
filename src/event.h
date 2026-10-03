/*
 * event.h - thin readiness-notification abstraction.
 *
 * The server is written against kqueue (macOS / BSD). On Linux the same
 * interface is backed by epoll, so the rest of the code never touches a
 * platform-specific API. Client sockets are registered "one-shot": after an
 * event fires the fd is disabled until ev_rearm() is called, which guarantees
 * that exactly one worker thread owns a connection at any time.
 */
#ifndef EVENT_H
#define EVENT_H

typedef struct ev_loop ev_loop;

ev_loop *ev_create(void);
void ev_destroy(ev_loop *loop);

/* Register fd for read readiness. oneshot=0 keeps it armed (listener/pipe). */
int ev_add(ev_loop *loop, int fd, int oneshot);

/* Re-enable a one-shot fd after a worker finished with it. */
int ev_rearm(ev_loop *loop, int fd);

/* Wait for up to max ready fds; returns count, 0 on timeout, -1 on error. */
int ev_wait(ev_loop *loop, int *fds, int max, int timeout_ms);

const char *ev_backend_name(void);

#endif
