/* test_http.c - unit tests for the pure HTTP helpers. */
#include <stdio.h>
#include <string.h>

#include "../src/http.h"

static int failures, checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

static parse_result parse(const char *raw, http_request *req) {
    return http_parse_request(raw, strlen(raw), req);
}

static void test_parse_basic(void) {
    http_request r;
    CHECK(parse("GET /index.html?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n", &r) == PARSE_OK);
    CHECK(strcmp(r.method, "GET") == 0);
    CHECK(strcmp(r.path, "/index.html") == 0);
    CHECK(strcmp(r.query, "a=1&b=2") == 0);
    CHECK(r.version_minor == 1);
    CHECK(r.keep_alive == 1);
    CHECK(r.content_length == 0);
}

static void test_parse_keepalive_rules(void) {
    http_request r;
    CHECK(parse("GET / HTTP/1.0\r\n\r\n", &r) == PARSE_OK && r.keep_alive == 0);
    CHECK(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &r) == PARSE_OK && r.keep_alive == 1);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &r) == PARSE_OK && r.keep_alive == 0);
}

static void test_parse_body(void) {
    http_request r;
    const char *raw = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello";
    CHECK(parse(raw, &r) == PARSE_OK);
    CHECK(r.content_length == 5 && memcmp(r.body, "hello", 5) == 0);
    CHECK(strcmp(r.content_type, "text/plain") == 0);
    /* Body not fully received yet. */
    CHECK(http_parse_request(raw, strlen(raw) - 2, &r) == PARSE_INCOMPLETE);
}

static void test_parse_errors(void) {
    http_request r;
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n", &r) == PARSE_INCOMPLETE);
    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &r) == PARSE_BAD_REQUEST);          /* missing Host */
    CHECK(parse("get / HTTP/1.1\r\nHost: x\r\n\r\n", &r) == PARSE_BAD_REQUEST); /* lowercase method */
    CHECK(parse("GET index HTTP/1.1\r\nHost: x\r\n\r\n", &r) == PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n", &r) == PARSE_BAD_VERSION);
    CHECK(parse("GET / FOO\r\nHost: x\r\n\r\n", &r) == PARSE_BAD_REQUEST);
    CHECK(parse("GET /%zz HTTP/1.1\r\nHost: x\r\n\r\n", &r) == PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nBad Header: y\r\n\r\n", &r) == PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", &r) == PARSE_BAD_REQUEST);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", &r) == PARSE_BAD_REQUEST);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 999999\r\n\r\n", &r) == PARSE_PAYLOAD_TOO_LARGE);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == PARSE_NOT_IMPLEMENTED);

    char big[MAX_HEADER_SIZE + 64];
    memset(big, 'a', sizeof big);
    memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
    CHECK(http_parse_request(big, sizeof big, &r) == PARSE_HEADERS_TOO_LARGE);

    char longuri[MAX_TARGET + 64];
    int n = snprintf(longuri, sizeof longuri, "GET /");
    memset(longuri + n, 'a', MAX_TARGET);
    strcpy(longuri + n + MAX_TARGET, " HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(parse(longuri, &r) == PARSE_URI_TOO_LONG);
}

static void test_url_decode(void) {
    char out[64];
    CHECK(http_url_decode("/a%20b", 6, out, sizeof out) == 0 && strcmp(out, "/a b") == 0);
    CHECK(http_url_decode("/%2e%2E/x", 9, out, sizeof out) == 0 && strcmp(out, "/../x") == 0);
    CHECK(http_url_decode("/a+b", 4, out, sizeof out) == 0 && strcmp(out, "/a+b") == 0);
    CHECK(http_url_decode("/%00", 4, out, sizeof out) == -1);
    CHECK(http_url_decode("/%0d%0a", 7, out, sizeof out) == -1);
    CHECK(http_url_decode("/%4", 3, out, sizeof out) == -1);
    CHECK(http_url_decode("/%g1", 4, out, sizeof out) == -1);
    CHECK(http_url_decode("/abcdef", 7, out, 4) == -1);
}

static void test_path_safety(void) {
    CHECK(http_path_is_safe("/"));
    CHECK(http_path_is_safe("/a/b.html"));
    CHECK(http_path_is_safe("/..hidden/file"));
    CHECK(!http_path_is_safe("/../etc/passwd"));
    CHECK(!http_path_is_safe("/a/../../etc/passwd"));
    CHECK(!http_path_is_safe("/a/.."));
    CHECK(!http_path_is_safe("/./a"));
    CHECK(!http_path_is_safe("/a\\..\\b"));
    CHECK(!http_path_is_safe("relative"));
}

static void test_mime(void) {
    CHECK(strncmp(http_mime_type("/index.html"), "text/html", 9) == 0);
    CHECK(strncmp(http_mime_type("/STYLE.CSS"), "text/css", 8) == 0);
    CHECK(strcmp(http_mime_type("/img/logo.png"), "image/png") == 0);
    CHECK(strcmp(http_mime_type("/noext"), "application/octet-stream") == 0);
    CHECK(strcmp(http_mime_type("/dir.d/noext"), "application/octet-stream") == 0);
}

static void test_format_headers(void) {
    char buf[512];
    int n = http_format_headers(buf, sizeof buf, 404, "text/plain", 12, 0, NULL);
    CHECK(n > 0);
    CHECK(strncmp(buf, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(buf, "Content-Length: 12\r\n") != NULL);
    CHECK(strstr(buf, "Connection: close\r\n") != NULL);
    CHECK(http_format_headers(buf, 16, 200, "text/plain", 1, 1, NULL) == -1);
}

int main(void) {
    test_parse_basic();
    test_parse_keepalive_rules();
    test_parse_body();
    test_parse_errors();
    test_url_decode();
    test_path_safety();
    test_mime();
    test_format_headers();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
