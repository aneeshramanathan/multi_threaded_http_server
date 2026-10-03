/* event_epoll.c - epoll backend for Linux, mirroring the kqueue semantics. */
#include "event.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#define EV_BATCH 256

struct ev_loop {
    int ep;
};

ev_loop *ev_create(void) {
    ev_loop *loop = malloc(sizeof *loop);
    if (!loop) return NULL;
    loop->ep = epoll_create1(EPOLL_CLOEXEC);
    if (loop->ep < 0) {
        free(loop);
        return NULL;
    }
    return loop;
}

void ev_destroy(ev_loop *loop) {
    if (!loop) return;
    close(loop->ep);
    free(loop);
}

int ev_add(ev_loop *loop, int fd, int oneshot) {
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLRDHUP | (oneshot ? EPOLLONESHOT : 0);
    ev.data.fd = fd;
    return epoll_ctl(loop->ep, EPOLL_CTL_ADD, fd, &ev);
}

int ev_rearm(ev_loop *loop, int fd) {
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
    ev.data.fd = fd;
    return epoll_ctl(loop->ep, EPOLL_CTL_MOD, fd, &ev);
}

int ev_wait(ev_loop *loop, int *fds, int max, int timeout_ms) {
    struct epoll_event events[EV_BATCH];
    if (max > EV_BATCH) max = EV_BATCH;

    int n = epoll_wait(loop->ep, events, max, timeout_ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) fds[i] = events[i].data.fd;
    return n;
}

const char *ev_backend_name(void) { return "epoll"; }
