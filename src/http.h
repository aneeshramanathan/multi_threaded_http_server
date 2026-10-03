/*
 * http.h - HTTP/1.x request parsing, URL decoding, MIME lookup and
 * response-header formatting. Everything here is pure (no I/O), which keeps
 * it unit-testable.
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define MAX_HEADER_SIZE 8192
#define MAX_BODY_SIZE (64 * 1024)
#define MAX_REQUEST_SIZE (MAX_HEADER_SIZE + MAX_BODY_SIZE)
#define MAX_HEADERS 64
#define MAX_TARGET 2048

typedef enum {
    PARSE_OK,
    PARSE_INCOMPLETE,
    PARSE_BAD_REQUEST,      /* 400 */
    PARSE_PAYLOAD_TOO_LARGE, /* 413 */
    PARSE_URI_TOO_LONG,     /* 414 */
    PARSE_HEADERS_TOO_LARGE, /* 431 */
    PARSE_NOT_IMPLEMENTED,  /* 501 */
    PARSE_BAD_VERSION       /* 505 */
} parse_result;

typedef struct {
    char method[16];
    char path[MAX_TARGET];  /* percent-decoded, query string removed */
    char query[MAX_TARGET]; /* raw query string, without the '?' */
    int version_minor;      /* HTTP/1.<minor> */
    int keep_alive;
    size_t content_length;
    char content_type[128];
    const char *body; /* points into the caller's buffer */
    size_t header_len; /* request line + headers + final CRLF */
} http_request;

parse_result http_parse_request(const char *buf, size_t len, http_request *req);
int http_status_for_parse_result(parse_result r);

/* Decode %XX escapes. Rejects malformed escapes and NUL bytes. 0 on success. */
int http_url_decode(const char *src, size_t len, char *dst, size_t dstsz);

/* 1 if the decoded path is absolute and contains no "." / ".." segments. */
int http_path_is_safe(const char *path);

const char *http_mime_type(const char *path);
const char *http_status_text(int status);

/* Writes a full response header block; returns length or -1 if truncated. */
int http_format_headers(char *out, size_t outsz, int status, const char *content_type,
                        size_t content_length, int keep_alive, const char *extra_headers);

#endif
