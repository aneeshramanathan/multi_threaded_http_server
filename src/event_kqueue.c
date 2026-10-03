/* event_kqueue.c - kqueue backend (macOS, FreeBSD, or Linux via libkqueue). */
#include "event.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define EV_BATCH 256

struct ev_loop {
    int kq;
};

ev_loop *ev_create(void) {
    ev_loop *loop = malloc(sizeof *loop);
    if (!loop) return NULL;
    loop->kq = kqueue();
    if (loop->kq < 0) {
        free(loop);
        return NULL;
    }
    return loop;
}

void ev_destroy(ev_loop *loop) {
    if (!loop) return;
    close(loop->kq);
    free(loop);
}

int ev_add(ev_loop *loop, int fd, int oneshot) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE | (oneshot ? EV_ONESHOT : 0), 0, 0, NULL);
    return kevent(loop->kq, &ev, 1, NULL, 0, NULL);
}

int ev_rearm(ev_loop *loop, int fd) {
    /* EV_ONESHOT deletes the filter once it fires, so re-adding re-arms it. */
    return ev_add(loop, fd, 1);
}

int ev_wait(ev_loop *loop, int *fds, int max, int timeout_ms) {
    struct kevent events[EV_BATCH];
    struct timespec ts = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
    if (max > EV_BATCH) max = EV_BATCH;

    int n = kevent(loop->kq, NULL, 0, events, max, timeout_ms < 0 ? NULL : &ts);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) fds[i] = (int)events[i].ident;
    return n;
}

const char *ev_backend_name(void) { return "kqueue"; }
