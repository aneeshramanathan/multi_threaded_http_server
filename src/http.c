/* http.c - request parsing and response helpers. */
#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *find_seq(const char *hay, size_t len, const char *needle, size_t nlen) {
    if (nlen > len) return NULL;
    for (size_t i = 0; i + nlen <= len; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) return hay + i;
    return NULL;
}

static int is_token_char(int c) {
    return isalnum(c) || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/* Case-insensitive search for a comma-separated token in a header value. */
static int header_has_token(const char *v, size_t vlen, const char *token) {
    size_t tlen = strlen(token);
    for (size_t i = 0; i + tlen <= vlen; i++)
        if (strncasecmp(v + i, token, tlen) == 0) return 1;
    return 0;
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int http_url_decode(const char *src, size_t len, char *dst, size_t dstsz) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '%') {
            if (i + 2 >= len) return -1;
            int hi = hex_val((unsigned char)src[i + 1]);
            int lo = hex_val((unsigned char)src[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (char)(hi * 16 + lo);
            i += 2;
        }
        /* Control bytes (NUL, CR, LF, ...) never belong in a path; rejecting
         * them blocks truncation tricks and header injection via Location. */
        if ((unsigned char)c < 0x20 || c == 0x7f) return -1;
        if (o + 1 >= dstsz) return -1;
        dst[o++] = c;
    }
    dst[o] = '\0';
    return 0;
}

int http_path_is_safe(const char *path) {
    if (path[0] != '/') return 0;
    if (strchr(path, '\\')) return 0;
    const char *seg = path + 1;
    for (;;) {
        const char *slash = strchr(seg, '/');
        size_t n = slash ? (size_t)(slash - seg) : strlen(seg);
        if ((n == 1 && seg[0] == '.') || (n == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
        if (!slash) break;
        seg = slash + 1;
    }
    return 1;
}

parse_result http_parse_request(const char *buf, size_t len, http_request *req) {
    memset(req, 0, sizeof *req);

    const char *end = find_seq(buf, len < MAX_HEADER_SIZE ? len : MAX_HEADER_SIZE, "\r\n\r\n", 4);
    if (!end) return len >= MAX_HEADER_SIZE ? PARSE_HEADERS_TOO_LARGE : PARSE_INCOMPLETE;
    req->header_len = (size_t)(end - buf) + 4;
    const char *hdr_stop = end + 2; /* one past the last header line's CRLF */

    /* --- request line: METHOD SP request-target SP HTTP-version --- */
    const char *line_end = find_seq(buf, (size_t)(hdr_stop - buf), "\r\n", 2);
    const char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (!sp1) return PARSE_BAD_REQUEST;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1));
    if (!sp2) return PARSE_BAD_REQUEST;

    size_t mlen = (size_t)(sp1 - buf);
    if (mlen == 0 || mlen >= sizeof req->method) return PARSE_BAD_REQUEST;
    for (size_t i = 0; i < mlen; i++)
        if (!isupper((unsigned char)buf[i])) return PARSE_BAD_REQUEST;
    memcpy(req->method, buf, mlen);

    const char *target = sp1 + 1;
    size_t tlen = (size_t)(sp2 - target);
    if (tlen == 0 || target[0] != '/') return PARSE_BAD_REQUEST;
    if (tlen >= MAX_TARGET) return PARSE_URI_TOO_LONG;

    const char *ver = sp2 + 1;
    size_t vlen = (size_t)(line_end - ver);
    if (vlen == 8 && memcmp(ver, "HTTP/1.", 7) == 0 && (ver[7] == '0' || ver[7] == '1'))
        req->version_minor = ver[7] - '0';
    else if (vlen >= 5 && memcmp(ver, "HTTP/", 5) == 0)
        return PARSE_BAD_VERSION;
    else
        return PARSE_BAD_REQUEST;

    for (size_t i = 0; i < tlen; i++)
        if ((unsigned char)target[i] <= 0x20 || target[i] == 0x7f) return PARSE_BAD_REQUEST;

    const char *q = memchr(target, '?', tlen);
    size_t plen = q ? (size_t)(q - target) : tlen;
    if (http_url_decode(target, plen, req->path, sizeof req->path) != 0) return PARSE_BAD_REQUEST;
    if (q) {
        size_t qlen = tlen - plen - 1;
        memcpy(req->query, q + 1, qlen);
        req->query[qlen] = '\0';
    }

    req->keep_alive = req->version_minor == 1;

    /* --- header fields --- */
    int nheaders = 0, have_host = 0, have_cl = 0;
    const char *p = line_end + 2;
    while (p < hdr_stop) {
        const char *le = find_seq(p, (size_t)(hdr_stop - p), "\r\n", 2);
        if (!le) return PARSE_BAD_REQUEST;
        if (++nheaders > MAX_HEADERS) return PARSE_HEADERS_TOO_LARGE;

        const char *colon = memchr(p, ':', (size_t)(le - p));
        if (!colon || colon == p) return PARSE_BAD_REQUEST;
        for (const char *c = p; c < colon; c++)
            if (!is_token_char((unsigned char)*c)) return PARSE_BAD_REQUEST;

        size_t nlen = (size_t)(colon - p);
        const char *v = colon + 1, *vend = le;
        while (v < vend && (*v == ' ' || *v == '\t')) v++;
        while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t')) vend--;
        size_t vlen2 = (size_t)(vend - v);

#define NAME_IS(s) (nlen == sizeof(s) - 1 && strncasecmp(p, s, nlen) == 0)
        if (NAME_IS("Host")) {
            have_host = 1;
        } else if (NAME_IS("Content-Length")) {
            if (vlen2 == 0 || vlen2 > 9) return vlen2 > 9 ? PARSE_PAYLOAD_TOO_LARGE : PARSE_BAD_REQUEST;
            size_t cl = 0;
            for (size_t i = 0; i < vlen2; i++) {
                if (!isdigit((unsigned char)v[i])) return PARSE_BAD_REQUEST;
                cl = cl * 10 + (size_t)(v[i] - '0');
            }
            if (have_cl && cl != req->content_length) return PARSE_BAD_REQUEST;
            have_cl = 1;
            req->content_length = cl;
        } else if (NAME_IS("Transfer-Encoding")) {
            return PARSE_NOT_IMPLEMENTED; /* chunked bodies unsupported; avoids smuggling */
        } else if (NAME_IS("Connection")) {
            if (header_has_token(v, vlen2, "close")) req->keep_alive = 0;
            else if (header_has_token(v, vlen2, "keep-alive")) req->keep_alive = 1;
        } else if (NAME_IS("Content-Type")) {
            size_t n = vlen2 < sizeof req->content_type - 1 ? vlen2 : sizeof req->content_type - 1;
            memcpy(req->content_type, v, n);
            req->content_type[n] = '\0';
        }
#undef NAME_IS
        p = le + 2;
    }

    if (req->version_minor == 1 && !have_host) return PARSE_BAD_REQUEST;
    if (req->content_length > MAX_BODY_SIZE) return PARSE_PAYLOAD_TOO_LARGE;
    if (len < req->header_len + req->content_length) return PARSE_INCOMPLETE;

    req->body = buf + req->header_len;
    return PARSE_OK;
}

int http_status_for_parse_result(parse_result r) {
    switch (r) {
    case PARSE_PAYLOAD_TOO_LARGE: return 413;
    case PARSE_URI_TOO_LONG: return 414;
    case PARSE_HEADERS_TOO_LARGE: return 431;
    case PARSE_NOT_IMPLEMENTED: return 501;
    case PARSE_BAD_VERSION: return 505;
    default: return 400;
    }
}

const char *http_mime_type(const char *path) {
    static const struct { const char *ext, *type; } types[] = {
        {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},   {"js", "text/javascript; charset=utf-8"},
        {"json", "application/json"},         {"txt", "text/plain; charset=utf-8"},
        {"xml", "application/xml"},           {"svg", "image/svg+xml"},
        {"png", "image/png"},                 {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},               {"gif", "image/gif"},
        {"webp", "image/webp"},               {"ico", "image/x-icon"},
        {"pdf", "application/pdf"},           {"wasm", "application/wasm"},
        {"woff2", "font/woff2"},              {"mp4", "video/mp4"},
    };
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash)) return "application/octet-stream";
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcasecmp(dot + 1, types[i].ext) == 0) return types[i].type;
    return "application/octet-stream";
}

const char *http_status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

int http_format_headers(char *out, size_t outsz, int status, const char *content_type,
                        size_t content_length, int keep_alive, const char *extra_headers) {
    int n = snprintf(out, outsz,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: mthttp\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "%s"
                     "\r\n",
                     status, http_status_text(status), content_type, content_length,
                     keep_alive ? "keep-alive" : "close", extra_headers ? extra_headers : "");
    return (n < 0 || (size_t)n >= outsz) ? -1 : n;
}
