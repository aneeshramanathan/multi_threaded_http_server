CC      ?= cc
CFLAGS  ?= -O2 -g
# Required flags live separately so `make CFLAGS=...` (e.g. sanitize) can't drop them.
ALL_CFLAGS  = -std=gnu11 -Wall -Wextra -Wpedantic -Wno-unused-parameter -pthread $(CFLAGS)
ALL_LDFLAGS = -pthread $(LDFLAGS)

UNAME_S := $(shell uname -s)

# kqueue on macOS/BSD, epoll on Linux. Override with `make BACKEND=kqueue`
# on Linux to build the kqueue path against libkqueue.
ifeq ($(UNAME_S),Linux)
  BACKEND ?= epoll
else
  BACKEND ?= kqueue
endif

ifeq ($(BACKEND),kqueue)
  ifeq ($(UNAME_S),Linux)
    ALL_CFLAGS += -I/usr/include/kqueue
    LDLIBS += -lkqueue
  endif
endif

BUILD := build
SRCS  := src/main.c src/server.c src/http.c src/router.c src/threadpool.c src/event_$(BACKEND).c
OBJS  := $(SRCS:src/%.c=$(BUILD)/%.o)

.PHONY: all test unit e2e bench run clean sanitize

all: $(BUILD)/mthttp $(BUILD)/loadgen

$(BUILD)/mthttp: $(OBJS)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/loadgen: tools/loadgen.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $<

$(BUILD)/test_http: tests/test_http.c src/http.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

unit: $(BUILD)/test_http
	./$(BUILD)/test_http

e2e: all
	bash tests/e2e.sh

test: unit e2e

# Build with AddressSanitizer + UBSan and run the full suite.
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	             LDFLAGS="-fsanitize=address,undefined"

bench: all
	./$(BUILD)/mthttp -p 8081 -d www & echo $$! > $(BUILD)/bench.pid; sleep 0.5; \
	./$(BUILD)/loadgen -p 8081 -c 100 -d 10 -u /health; rc=$$?; \
	kill -TERM $$(cat $(BUILD)/bench.pid); wait; exit $$rc

run: all
	./$(BUILD)/mthttp -p 8080 -d www

clean:
	rm -rf $(BUILD)
