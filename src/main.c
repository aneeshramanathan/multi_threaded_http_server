/* main.c - command-line parsing and signal setup. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

static void on_signal(int sig) {
    (void)sig;
    server_request_shutdown();
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p port] [-t threads] [-d docroot] [-i idle_timeout_sec] [-v]\n"
            "  -p  port to listen on            (default 8080)\n"
            "  -t  worker threads               (default: online CPUs * 2)\n"
            "  -d  directory to serve           (default ./www)\n"
            "  -i  keep-alive idle timeout, sec (default 30)\n"
            "  -v  log each request to stderr\n",
            prog);
}

int main(int argc, char **argv) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    server_config cfg = {
        .port = 8080,
        .threads = cpus > 0 ? (int)cpus * 2 : 8,
        .docroot = "./www",
        .idle_timeout_sec = 30,
        .verbose = 0,
    };

    int opt;
    while ((opt = getopt(argc, argv, "p:t:d:i:vh")) != -1) {
        switch (opt) {
        case 'p': cfg.port = atoi(optarg); break;
        case 't': cfg.threads = atoi(optarg); break;
        case 'd': cfg.docroot = optarg; break;
        case 'i': cfg.idle_timeout_sec = atoi(optarg); break;
        case 'v': cfg.verbose = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (cfg.port <= 0 || cfg.port > 65535 || cfg.threads <= 0 || cfg.threads > 1024 ||
        cfg.idle_timeout_sec <= 0) {
        usage(argv[0]);
        return 2;
    }

    /* A peer closing mid-write must not kill the process. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    return server_run(&cfg);
}
