# multi_threaded_http_server

A non-blocking, multithreaded HTTP/1.1 server in C, built on **kqueue** (macOS/BSD) with an
**epoll** backend for Linux, and a **pthreads** worker pool synchronized with a mutex and
condition variable.

## Features

- Event-loop + thread-pool architecture with one-shot readiness notifications
- HTTP/1.1 request parsing with keep-alive and pipelining
- Static file serving with MIME detection, `HEAD` support and directory index handling
- URL (percent) decoding
- Path-traversal protection: segment checks plus `realpath()` containment, so symlinks can't escape the docroot
- Strict input limits: 400 / 413 / 414 / 431 / 501 / 505 responses for malformed or oversized requests
- Idle keep-alive connection reaping
- Graceful shutdown on `SIGINT`/`SIGTERM`: stops accepting, drains in-flight work, closes idle connections
- API endpoints: `GET /health`, `GET /api/stats`, `POST /api/echo`

## Build and run

```sh
make            # builds build/mthttp and build/loadgen (kqueue on macOS, epoll on Linux)
make run        # serves ./www on http://localhost:8080
./build/mthttp -p 8080 -t 8 -d ./www -i 30 -v
```

| Flag | Meaning | Default |
|------|---------|---------|
| `-p` | port | 8080 |
| `-t` | worker threads | 2 × CPUs |
| `-d` | docroot | `./www` |
| `-i` | keep-alive idle timeout (s) | 30 |
| `-v` | log each request | off |

On Linux, `make BACKEND=kqueue` builds the kqueue backend against `libkqueue-dev`.

## Tests

```sh
make unit       # parser / URL decoding / path safety / MIME unit tests
make e2e        # starts the server, runs ~30 HTTP checks, a load burst and shutdown checks
make sanitize   # full suite under AddressSanitizer + UndefinedBehaviorSanitizer
make bench      # 100 concurrent keep-alive connections for 10 s
```

CI (`.github/workflows/ci.yml`) runs everything on macOS (kqueue), Ubuntu (epoll) and Ubuntu (kqueue via libkqueue).

## Benchmark

`make bench` on GitHub Actions runners, with the load generator and server on the same machine,
100 concurrent keep-alive connections for 10 seconds, `GET /health`:

| Runner / backend | Throughput | Failures | Avg latency |
|------------------|-----------:|---------:|------------:|
| macOS / kqueue   | ~87,600 req/s | 0 | 1.14 ms |
| Ubuntu / epoll   | ~98,300 req/s | 0 | 1.02 ms |
| Ubuntu / kqueue (libkqueue) | ~89,600 req/s | 0 | 1.12 ms |

## Architecture

```
            ┌──────────────── event-loop thread ────────────────┐
 clients ──▶│ kqueue/epoll wait ─▶ accept() new sockets          │
            │                   ─▶ ready client fd ─┐            │
            │ idle reaper, shutdown pipe            │            │
            └───────────────────────────────────────┼────────────┘
                                                    ▼
                                   bounded queue (mutex + condvar)
                                                    ▼
            ┌────────────── N worker threads (pthreads) ─────────┐
            │ recv ─▶ parse ─▶ route (API / static file) ─▶ send │
            │ then re-arm the fd (keep-alive) or close it        │
            └────────────────────────────────────────────────────┘
```

Client sockets are registered **one-shot** (`EV_ONESHOT` / `EPOLLONESHOT`). Once the loop hands
a socket to a worker it can't fire again until that worker re-arms it, so each connection has
exactly one owner at a time. Only the shared connection table needs a lock.

| File | Responsibility |
|------|----------------|
| `src/main.c` | CLI flags, signal handlers |
| `src/server.c` | listener, event loop, connection lifecycle, graceful shutdown |
| `src/event_kqueue.c`, `src/event_epoll.c` | readiness backends behind `src/event.h` |
| `src/threadpool.c` | worker pool and bounded task queue |
| `src/http.c` | request parser, URL decoding, MIME types, response headers |
| `src/router.c` | endpoints, static files, path-traversal checks, socket writes |
| `tools/loadgen.c` | multithreaded keep-alive load generator |
| `tests/` | unit tests and end-to-end test script |
