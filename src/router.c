/* router.c - request routing, static file serving and API endpoints. */
#include "router.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SIGPIPE is ignored process-wide instead */
#endif

#define WRITE_TIMEOUT_MS 5000
#define FILE_CHUNK 65536

static char g_docroot[PATH_MAX];
static size_t g_docroot_len;

void router_init(const char *docroot) {
    snprintf(g_docroot, sizeof g_docroot, "%s", docroot);
    g_docroot_len = strlen(g_docroot);
}

int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Socket buffer full: wait until the peer drains it. */
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, WRITE_TIMEOUT_MS) <= 0) return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int send_response(int fd, int status, const char *ctype, const char *body, size_t body_len,
                         int keep_alive, const char *extra, int head_only) {
    char hdr[1024];
    int hlen = http_format_headers(hdr, sizeof hdr, status, ctype, body_len, keep_alive, extra);
    if (hlen < 0) return -1;
    if (head_only || body_len == 0) return write_all(fd, hdr, (size_t)hlen);

    /* Small bodies go out in a single send to avoid an extra packet. */
    if ((size_t)hlen + body_len <= 4096) {
        char out[4096];
        memcpy(out, hdr, (size_t)hlen);
        memcpy(out + hlen, body, body_len);
        return write_all(fd, out, (size_t)hlen + body_len);
    }
    if (write_all(fd, hdr, (size_t)hlen) < 0) return -1;
    return write_all(fd, body, body_len);
}

int send_error(int fd, int status, int keep_alive) {
    char body[256];
    int n = snprintf(body, sizeof body, "{\"error\":%d,\"message\":\"%s\"}\n", status,
                     http_status_text(status));
    const char *extra = status == 405 ? "Allow: GET, HEAD, POST\r\n" : NULL;
    return send_response(fd, status, "application/json", body, (size_t)n, keep_alive, extra, 0);
}

static int serve_file(int fd, const http_request *req, int keep_alive, int head_only) {
    if (!http_path_is_safe(req->path)) return send_error(fd, 403, keep_alive);

    char full[PATH_MAX];
    size_t plen = strlen(req->path);
    const char *suffix = req->path[plen - 1] == '/' ? "index.html" : "";
    if (snprintf(full, sizeof full, "%s%s%s", g_docroot, req->path, suffix) >= (int)sizeof full)
        return send_error(fd, 414, keep_alive);

    /* Resolve symlinks and confirm the result is still inside the docroot. */
    char resolved[PATH_MAX];
    if (!realpath(full, resolved))
        return send_error(fd, (errno == EACCES) ? 403 : 404, keep_alive);
    if (strncmp(resolved, g_docroot, g_docroot_len) != 0 ||
        (resolved[g_docroot_len] != '/' && resolved[g_docroot_len] != '\0'))
        return send_error(fd, 403, keep_alive);

    struct stat st;
    if (stat(resolved, &st) < 0) return send_error(fd, 404, keep_alive);
    if (S_ISDIR(st.st_mode)) {
        /* Redirect /dir -> /dir/ so relative links in index.html resolve. */
        char loc[MAX_TARGET + 32];
        snprintf(loc, sizeof loc, "Location: %s/\r\n", req->path);
        return send_response(fd, 301, "text/plain; charset=utf-8", "", 0, keep_alive, loc, 0);
    }
    if (!S_ISREG(st.st_mode)) return send_error(fd, 403, keep_alive);

    int file = open(resolved, O_RDONLY);
    if (file < 0) return send_error(fd, errno == EACCES ? 403 : 404, keep_alive);

    char hdr[1024];
    int hlen = http_format_headers(hdr, sizeof hdr, 200, http_mime_type(resolved),
                                   (size_t)st.st_size, keep_alive, NULL);
    int rc = write_all(fd, hdr, (size_t)hlen);

    if (rc == 0 && !head_only) {
        char *chunk = malloc(FILE_CHUNK);
        off_t remaining = st.st_size;
        while (rc == 0 && chunk && remaining > 0) {
            ssize_t n = read(file, chunk, FILE_CHUNK);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                rc = -1; /* file shrank underneath us; headers already sent */
                break;
            }
            rc = write_all(fd, chunk, (size_t)n);
            remaining -= n;
        }
        if (!chunk) rc = -1;
        free(chunk);
    }
    close(file);
    return rc;
}

static int handle_stats(int fd, int keep_alive, int head_only) {
    server_stats s;
    server_get_stats(&s);
    char body[512];
    int n = snprintf(body, sizeof body,
                     "{\"requests_total\":%llu,\"active_connections\":%llu,"
                     "\"worker_threads\":%d,\"event_backend\":\"%s\",\"uptime_seconds\":%ld}\n",
                     s.requests_total, s.active_connections, s.worker_threads, s.backend,
                     s.uptime_seconds);
    return send_response(fd, 200, "application/json", body, (size_t)n, keep_alive, NULL, head_only);
}

int router_handle(int fd, const http_request *req, int keep_alive) {
    int is_get = strcmp(req->method, "GET") == 0;
    int is_head = strcmp(req->method, "HEAD") == 0;
    int is_post = strcmp(req->method, "POST") == 0;

    if (strcmp(req->path, "/health") == 0) {
        if (!is_get && !is_head) return send_error(fd, 405, keep_alive);
        static const char ok[] = "{\"status\":\"ok\"}\n";
        return send_response(fd, 200, "application/json", ok, sizeof ok - 1, keep_alive, NULL, is_head);
    }
    if (strcmp(req->path, "/api/stats") == 0) {
        if (!is_get && !is_head) return send_error(fd, 405, keep_alive);
        return handle_stats(fd, keep_alive, is_head);
    }
    if (strcmp(req->path, "/api/echo") == 0) {
        if (!is_post) return send_error(fd, 405, keep_alive);
        const char *ctype = req->content_type[0] ? req->content_type : "application/octet-stream";
        return send_response(fd, 200, ctype, req->body, req->content_length, keep_alive, NULL, 0);
    }

    if (!is_get && !is_head) return send_error(fd, 405, keep_alive);
    return serve_file(fd, req, keep_alive, is_head);
}
