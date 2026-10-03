/*
 * server.h - listener socket, event loop and connection lifecycle.
 */
#ifndef SERVER_H
#define SERVER_H

typedef struct {
    int port;
    int threads;
    const char *docroot;
    int idle_timeout_sec;
    int verbose;
} server_config;

typedef struct {
    unsigned long long requests_total;
    unsigned long long active_connections;
    int worker_threads;
    const char *backend;
    long uptime_seconds;
} server_stats;

/* Blocks until shutdown is requested; returns 0 on clean shutdown. */
int server_run(const server_config *cfg);

/* Async-signal-safe: wakes the event loop and begins graceful shutdown. */
void server_request_shutdown(void);

void server_get_stats(server_stats *out);

#endif
