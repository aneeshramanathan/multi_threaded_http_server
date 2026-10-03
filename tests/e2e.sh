#!/usr/bin/env bash
# End-to-end tests: start the real server binary, hit it with curl and raw
# sockets, run a concurrent load burst, then verify graceful shutdown.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mthttp"
LOADGEN="$ROOT/build/loadgen"
PORT="${PORT:-$((20000 + RANDOM % 20000))}"
BASE="http://127.0.0.1:$PORT"

TMP="$(mktemp -d)"
DOCROOT="$TMP/www"
LOG="$TMP/server.log"
cp -R "$ROOT/www" "$DOCROOT"
mkdir -p "$DOCROOT/sub"
echo "nested" > "$DOCROOT/sub/index.html"
ln -s /etc "$DOCROOT/escape"                       # symlink pointing outside docroot
head -c 2097152 /dev/urandom > "$DOCROOT/big.bin"   # 2 MiB binary file

pass=0
fail=0
ok()   { pass=$((pass + 1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
expect() { # name expected actual
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi
}
status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

# Send raw bytes over a TCP socket and print the response. Every raw request
# below ends in a response that closes the connection, so cat terminates.
raw() {
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    printf "$1" >&3
    cat <&3
    exec 3<&-
}
sha256() { if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi | cut -d' ' -f1; }

cleanup() { kill "$SERVER_PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

"$BIN" -p "$PORT" -t 8 -d "$DOCROOT" -i 5 >"$LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$BASE/health" && break
    sleep 0.1
done

echo "== functional =="
expect "GET / returns 200"                  200 "$(status "$BASE/")"
expect "index is text/html"                 "text/html; charset=utf-8" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/")"
expect "CSS MIME type"                      "text/css; charset=utf-8" "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/style.css")"
expect "/health body"                       '{"status":"ok"}' "$(curl -s "$BASE/health")"
expect "HEAD has no body"                   0 "$(curl -s -I "$BASE/" -o /dev/null -w '%{size_download}')"
expect "missing file is 404"                404 "$(status "$BASE/nope.html")"
expect "directory redirects"                301 "$(status "$BASE/sub")"
expect "directory serves index.html"        nested "$(curl -s "$BASE/sub/")"
expect "URL-decoded path"                   200 "$(status "$BASE/%69ndex.html")"
expect "POST /api/echo echoes body"         "hello world" "$(curl -s -H 'Content-Type: text/plain' --data-binary 'hello world' "$BASE/api/echo")"
expect "DELETE is 405"                      405 "$(status -X DELETE "$BASE/")"
expect "GET /api/echo is 405"               405 "$(status "$BASE/api/echo")"
curl -s "$BASE/api/stats" | grep -q '"requests_total":' && ok "/api/stats returns JSON" || bad "/api/stats returns JSON"

echo "== security =="
expect "../ traversal blocked"              403 "$(status --path-as-is "$BASE/../../etc/passwd")"
expect "encoded %2e%2e traversal blocked"   403 "$(status --path-as-is "$BASE/%2e%2e/%2e%2e/etc/passwd")"
expect "symlink escape blocked"             403 "$(status "$BASE/escape/passwd")"
expect "encoded NUL rejected"               400 "$(status "$BASE/index.html%00.png")"
expect "CRLF injection rejected"            400 "$(status "$BASE/sub%0d%0aSet-Cookie:x")"
expect "chunked body not implemented"       501 "$(status -H 'Transfer-Encoding: chunked' --data-binary x "$BASE/api/echo")"
expect "oversized body is 413"              413 "$(head -c 70000 /dev/zero | status --data-binary @- "$BASE/api/echo")"
expect "malformed request is 400"           "HTTP/1.1 400 Bad Request" "$(raw 'GARBAGE\r\n\r\n' | head -1 | tr -d '\r')"
expect "HTTP/2.0 request line is 505"       "HTTP/1.1 505 HTTP Version Not Supported" "$(raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n' | head -1 | tr -d '\r')"

echo "== connections =="
reused=$(curl -sv "$BASE/health" "$BASE/health" -o /dev/null -o /dev/null 2>&1 | grep -Eci 're-?using existing')
[ "$reused" -ge 1 ] && ok "keep-alive reuses the connection" || bad "keep-alive reuses the connection"
pipelined=$(raw 'GET /health HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | grep -c '^HTTP/1.1 200')
expect "pipelined requests both answered"   2 "$pipelined"
expect "2 MiB file is byte-identical" \
    "$(sha256 < "$DOCROOT/big.bin")" "$(curl -s "$BASE/big.bin" | sha256)"

echo "== concurrency (100 connections, 3s) =="
if "$LOADGEN" -p "$PORT" -c 100 -d 3 -u /health > "$TMP/load.txt"; then
    ok "load test with zero failures"
else
    bad "load test reported failures"
fi
sed 's/^/    /' "$TMP/load.txt"

echo "== graceful shutdown =="
exec 4<>"/dev/tcp/127.0.0.1/$PORT"   # hold an idle keep-alive connection open
kill -TERM "$SERVER_PID"
wait "$SERVER_PID"
expect "SIGTERM exits with status 0"        0 "$?"
grep -q "shutdown complete" "$LOG" && ok "shutdown log message" || bad "shutdown log message"
exec 4<&-
curl -s -o /dev/null "$BASE/health" && bad "port closed after shutdown" || ok "port closed after shutdown"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
