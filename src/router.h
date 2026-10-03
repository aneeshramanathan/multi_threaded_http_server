/*
 * router.h - maps parsed requests to handlers (API endpoints and static
 * files) and writes the response to the client socket.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>

#include "http.h"

/* docroot must already be an absolute, canonical path (from realpath). */
void router_init(const char *docroot);

/* Returns 0 on success, -1 if the socket write failed. */
int router_handle(int fd, const http_request *req, int keep_alive);

int send_error(int fd, int status, int keep_alive);

/* Write the full buffer to a non-blocking socket, waiting on POLLOUT. */
int write_all(int fd, const void *buf, size_t len);

#endif
