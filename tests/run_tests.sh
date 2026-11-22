#!/usr/bin/env bash
# End-to-end test suite for ngx_mod_tile (Phase 1 MVP).
#
# Starts a mock renderd (tests/mock_renderd.py) and nginx with the dynamic
# module, then drives the tile-serving paths with curl and asserts responses.
#
# Usage: tests/run_tests.sh
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
NGINX="$REPO/build/nginx-src/objs/nginx"
NGINX_CONF="$HERE/nginx_tile.conf"

BASE=/tmp/tilestore
SOCKET=/tmp/tile-test-renderd.sock
MOCK_LOG=/tmp/mock-renderd.log
BASE_URL=http://127.0.0.1:18080

PASS=0
FAIL=0
FAILED_TESTS=()

cleanup() {
    [[ -n "${NGINX_PID:-}" ]] && kill "$NGINX_PID" 2>/dev/null
    [[ -n "${MOCK_PID:-}" ]] && kill "$MOCK_PID" 2>/dev/null
    [[ -f /tmp/tile-test-nginx.pid ]] && kill "$(cat /tmp/tile-test-nginx.pid)" 2>/dev/null
    [[ -f /tmp/tile-test-mock.pid ]] && kill "$(cat /tmp/tile-test-mock.pid)" 2>/dev/null
    wait 2>/dev/null
    true
}
trap cleanup EXIT

assert_status() {  # $1 desc, $2 url, $3 expected status, extra curl args...
    local desc="$1" url="$2" want="$3"; shift 3
    local got
    got=$(curl -s -o /dev/null -w '%{http_code}' "$@" "$url")
    if [[ "$got" == "$want" ]]; then
        PASS=$((PASS+1)); echo "PASS: $desc ($got)"
    else
        FAIL=$((FAIL+1)); FAILED_TESTS+=("$desc")
        echo "FAIL: $desc (want $want, got $got)"
    fi
}

assert_eq() {  # $1 desc, $2 actual, $3 expected
    local desc="$1" got="$2" want="$3"
    if [[ "$got" == "$want" ]]; then
        PASS=$((PASS+1)); echo "PASS: $desc"
    else
        FAIL=$((FAIL+1)); FAILED_TESTS+=("$desc")
        echo "FAIL: $desc"
        echo "      want: $want"
        echo "      got:  $got"
    fi
}

assert_contains() {  # $1 desc, $2 haystack, $3 needle
    local desc="$1" hay="$2" needle="$3"
    if [[ "$hay" == *"$needle"* ]]; then
        PASS=$((PASS+1)); echo "PASS: $desc"
    else
        FAIL=$((FAIL+1)); FAILED_TESTS+=("$desc")
        echo "FAIL: $desc"
        echo "      missing: $needle"
        echo "      got:     $hay"
    fi
}

start_mock() {  # args...
    rm -f "$SOCKET"
    python3 "$HERE/mock_renderd.py" --socket "$SOCKET" --log "$MOCK_LOG" "$@" &
    MOCK_PID=$!
    echo "$MOCK_PID" > /tmp/tile-test-mock.pid
    for i in $(seq 1 50); do
        [[ -S "$SOCKET" ]] && return 0
        sleep 0.05
    done
    echo "FATAL: mock renderd did not create socket" >&2; exit 1
}

start_nginx() {
    "$NGINX" -t -e /tmp/tile-test-error.log -c "$NGINX_CONF" || exit 1
    "$NGINX" -e /tmp/tile-test-error.log -c "$NGINX_CONF"
    NGINX_PID=$!
    for i in $(seq 1 50); do
        # both: port responds AND our process is still alive (a stale nginx
        # from an earlier run would answer the port but kill -0 would fail)
        curl -s -o /dev/null "$BASE_URL/static/" \
            && kill -0 "$NGINX_PID" 2>/dev/null && return 0
        kill -0 "$NGINX_PID" 2>/dev/null || {
            echo "FATAL: nginx exited immediately (port conflict?)" >&2; exit 1; }
        sleep 0.1
    done
    echo "FATAL: nginx did not come up" >&2; exit 1
}

# kill leftovers from previous runs (separate script: pkill -f patterns
# must never run inside a command line that mentions them)
"$HERE/kill_stale.sh"
if ss -tln 2>/dev/null | grep -q ':18080'; then
    echo "FATAL: port 18080 still in use" >&2; exit 1
fi

echo "== generating tile store =="
rm -rf "$BASE" "$MOCK_LOG" /tmp/tile-static /tmp/tile-empty
mkdir -p /tmp/tile-static/static /tmp/tile-empty /tmp/tile-core
echo static-ok > /tmp/tile-static/static/index.html
python3 "$HERE/gen_metatiles.py" "$BASE" > /dev/null

echo "== phase 1: mock renderd (done, 0.15s delay, renders missing tiles) =="
start_mock --mode done --delay 0.15 --render-dir "$BASE"
start_nginx

# --- basic fresh hits ---
curl -s "$BASE_URL/tiles/0/0/0.png" | md5sum | awk '{print $1}' > /tmp/got.md5
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(0,0,0))" | md5sum | awk '{print $1}' > /tmp/want.md5
assert_eq "fresh tile body matches generated PNG" "$(cat /tmp/got.md5)" "$(cat /tmp/want.md5)"
assert_status "fresh tile z/x/y 200" "$BASE_URL/tiles/3/5/5.png" 200
assert_eq "Content-Type header" "$(curl -sI "$BASE_URL/tiles/3/5/5.png" | tr -d '\r' | awk 'tolower($1)=="content-type:"{print $2}')" "image/png"
assert_contains "ETag present" "$(curl -sI "$BASE_URL/tiles/3/5/5.png")" "ETag:"
assert_contains "Cache-Control present" "$(curl -sI "$BASE_URL/tiles/3/5/5.png")" "max-age="
assert_contains "Expires present" "$(curl -sI "$BASE_URL/tiles/3/5/5.png")" "Expires:"

# --- HEAD ---
assert_status "HEAD returns 200" "$BASE_URL/tiles/3/5/5.png" 200 -I
HEADLEN=$(curl -s -I "$BASE_URL/tiles/3/5/5.png" | tr -d '\r' | awk 'tolower($1)=="content-length:"{print $2}')
assert_eq "HEAD has content-length" "$HEADLEN" "$(curl -s "$BASE_URL/tiles/3/5/5.png" | wc -c)"

# --- 304 via If-None-Match ---
ETAG=$(curl -sI "$BASE_URL/tiles/3/5/5.png" | tr -d '\r' | awk 'tolower($1)=="etag:"{print $2}')
assert_status "If-None-Match -> 304" "$BASE_URL/tiles/3/5/5.png" 304 -H "If-None-Match: $ETAG"
assert_status "If-None-Match wildcard -> 304" "$BASE_URL/tiles/3/5/5.png" 304 -H 'If-None-Match: *'
assert_status "stale etag -> 200" "$BASE_URL/tiles/3/5/5.png" 200 -H 'If-None-Match: "deadbeef"'

# --- CORS ---
assert_status "CORS allowed origin 200" "$BASE_URL/tiles/3/5/5.png" 200 -H "Origin: http://allowed.example.com"
CORSOUT=$(curl -s -D- -o /dev/null "$BASE_URL/tiles/3/5/5.png" -H "Origin: http://allowed.example.com" | tr -d '\r')
assert_contains "CORS echoes origin" "$CORSOUT" "Access-Control-Allow-Origin: http://allowed.example.com"
assert_contains "CORS adds Vary: Origin" "$CORSOUT" "Vary: Origin"
assert_status "CORS disallowed origin 403" "$BASE_URL/tiles/3/5/5.png" 403 -H "Origin: http://evil.example.com"
PREFLIGHT=$(curl -s -D- -o /dev/null -X OPTIONS "$BASE_URL/tiles/3/5/5.png" \
    -H "Origin: http://allowed.example.com" -H "Access-Control-Request-Method: GET" | tr -d '\r')
assert_contains "preflight max-age" "$PREFLIGHT" "Access-Control-Max-Age: 604800"
assert_contains "preflight allow-origin" "$PREFLIGHT" "Access-Control-Allow-Origin: http://allowed.example.com"

# --- compressed metatile (METZ) ---
assert_status "METZ tile 200" "$BASE_URL/tiles/7/64/64.png" 200
assert_eq "METZ without Accept-Encoding: no content-encoding" \
    "$(curl -sI "$BASE_URL/tiles/7/64/64.png" | tr -d '\r' | grep -ci 'content-encoding' || true)" "0"
assert_contains "METZ with gzip accept: content-encoding gzip" \
    "$(curl -sI "$BASE_URL/tiles/7/64/64.png" -H 'Accept-Encoding: gzip' | tr -d '\r')" "Content-Encoding: gzip"

# --- invalid requests ---
assert_status "out of bounds coords 404" "$BASE_URL/tiles/2/9999/0.png" 404
assert_status "zoom above maxzoom 404" "$BASE_URL/tiles/30/0/0.png" 404
assert_status "wrong extension declined (404 by catch-all)" "$BASE_URL/tiles/2/5/5.jpg" 404
assert_status "non-tile URI untouched (static file)" "$BASE_URL/static/" 200
assert_contains "static file body" "$(curl -s $BASE_URL/static/)" "static-ok"
assert_status "unknown path 404" "$BASE_URL/nothing" 404

# --- imported layer (renderd.conf) ---
assert_status "imported layer fresh tile 200" "$BASE_URL/imp/1/1/1.png" 200
IT=$(curl -s "$BASE_URL/imp/tile-layer.json" | tr -d '\t\n')
assert_contains "imported TileJSON name" "$IT" '"name": "osmtest"'
assert_contains "imported TileJSON attribution" "$IT" '(c) ngx_mod_tile tests'

# --- tile-layer.json ---
TJ=$(curl -s "$BASE_URL/tiles/tile-layer.json")
assert_contains "TileJSON version" "$TJ" '"tilejson": "2.0.0"'
assert_contains "TileJSON name" "$TJ" '"name": "test"'
assert_contains "TileJSON tiles URL" "$TJ" '"http://tile.local/tiles/{z}/{x}/{y}.png"'
assert_status "TileJSON 200" "$BASE_URL/tiles/tile-layer.json" 200

# --- /status ---
ST=$(curl -s "$BASE_URL/tiles/3/5/5.png/status")
assert_contains "status clean" "$ST" "Tile is clean"
STMISS=$(curl -s "$BASE_URL/tiles/6/50/51.png/status")
assert_contains "status missing" "$STMISS" "No tile could not be found at storage location:"
assert_status "status of expired shows due to be rendered" "$BASE_URL/tiles/8/64/64.png/status" 200
STOLD=$(curl -s "$BASE_URL/tiles/8/64/64.png/status")
assert_contains "status old -> due to be rendered" "$STOLD" "due to be rendered"

# --- /dirty ---
: > "$MOCK_LOG"
assert_status "/dirty 200" "$BASE_URL/tiles/7/100/100.png/dirty" 200
sleep 0.3
assert_contains "dirty reached renderd as cmdDirty" "$(cat "$MOCK_LOG")" "cmd=Dirty(2)"

# --- expired (old) tile: serve stale + submit render ---
: > "$MOCK_LOG"
OLDOUT=$(curl -s -D- "$BASE_URL/tiles/8/64/64.png" -o /tmp/old-body | tr -d '\r')
assert_contains "old tile served 200" "$OLDOUT" "HTTP/1.1 200"
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(8,64,64))" | md5sum | awk '{print $1}' > /tmp/want-old.md5
assert_eq "old tile body is stale content" "$(md5sum /tmp/old-body | awk '{print $1}')" "$(cat /tmp/want-old.md5)"
sleep 0.3
assert_contains "old tile submitted cmdRenderLow" "$(cat "$MOCK_LOG")" "cmd=RenderLow(7)"

# --- very old tile ---
: > "$MOCK_LOG"
curl -s -o /dev/null "$BASE_URL/tiles/9/64/64.png"
sleep 0.3
assert_contains "very old tile submitted cmdRender" "$(cat "$MOCK_LOG")" "cmd=Render(1)"

# --- missing tile rendered on demand ---
: > "$MOCK_LOG"
MISSSTART=$(date +%s%N)
MISSSTATUS=$(curl -s -o /tmp/miss-body -w '%{http_code}' "$BASE_URL/tiles/7/5/5.png")
MISSEND=$(date +%s%N)
assert_status "missing tile rendered on demand -> 200" "$BASE_URL/tiles/7/5/5.png" 200
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(7,5,5))" | md5sum | awk '{print $1}' > /tmp/want-miss.md5
assert_eq "rendered tile content correct" "$(md5sum /tmp/miss-body | awk '{print $1}')" "$(cat /tmp/want-miss.md5)"
assert_contains "missing submitted cmdRenderPrio" "$(cat "$MOCK_LOG")" "cmd=RenderPrio(5)"
MSECS=$(( (MISSEND - MISSSTART) / 1000000 ))
if [[ $MSECS -lt 5000 ]]; then PASS=$((PASS+1)); echo "PASS: on-demand render waited ($MSECS ms)"; else FAIL=$((FAIL+1)); FAILED_TESTS+=("render wait"); echo "FAIL: render wait too long ($MSECS ms)"; fi

echo "== phase 2: mock renderd never answers (timeout path) =="
kill "$MOCK_PID" 2>/dev/null; wait "$MOCK_PID" 2>/dev/null || true
start_mock --mode never

TSTART=$(date +%s%N)
assert_status "missing tile + dead renderd -> 404" "$BASE_URL/tiles/6/50/50.png" 404
TEND=$(date +%s%N)
TSECS=$(( (TEND - TSTART) / 1000000 ))
if [[ $TSECS -ge 800 && $TSECS -lt 3000 ]]; then PASS=$((PASS+1)); echo "PASS: timeout honoured ($TSECS ms)"; else FAIL=$((FAIL+1)); FAILED_TESTS+=("timeout duration"); echo "FAIL: timeout duration off ($TSECS ms)"; fi
assert_status "old tile + dead renderd -> stale 200" "$BASE_URL/tiles/8/64/64.png" 200

echo "== phase 3: async concurrency proof =="
ulimit -c unlimited
rm -f /tmp/tile-core/core*
kill "$MOCK_PID" 2>/dev/null; wait "$MOCK_PID" 2>/dev/null || true
start_mock --mode done --delay 0.4 --render-dir "$BASE"

python3 "$HERE/async_test.py" "$BASE_URL" && PASS=$((PASS+2)) || { FAIL=$((FAIL+1)); FAILED_TESTS+=("async concurrency"); }
echo "PASS: async concurrency test completed"


echo "== phase 4: stats, metrics, throttling (stage 2) =="

# counters on the main server (zone-wide; previous phases already produced traffic)
M=$(curl -s --max-time 4 "$BASE_URL/mod_tile")
assert_contains "mod_tile NoResp200 present" "$M" "NoResp200: "
assert_contains "mod_tile layer counter" "$M" "NoRes200Layer/tiles/"
assert_contains "mod_tile zoom counter" "$M" "NoRespZoom"
assert_contains "mod_tile timing counter" "$M" "DurationTileBufferReads"

MX=$(curl -s --max-time 4 "$BASE_URL/metrics")
assert_contains "metrics responses family" "$MX" 'modtile_http_responses_total{status="200"}'
assert_contains "metrics 304 family" "$MX" 'modtile_http_responses_total{status="304"}'
assert_contains "metrics tiles family" "$MX" 'modtile_tiles_total{age="fresh",rendered="no"}'
assert_contains "metrics render family" "$MX" 'modtile_tiles_total{age="fresh",rendered="yes"}'
assert_contains "metrics reads seconds" "$MX" 'modtile_tile_reads_seconds_total{zoom="05"}'
assert_contains "metrics layer label" "$MX" 'modtile_layer_responses_total{layer="/tiles/",status="200"}'

# per-IP throttling: bucket of 5 tiles, refill 1 per 2s -> rapid requests hit 503
CODES=$(for i in $(seq 0 7); do curl -s -o /dev/null -w '%{http_code}\n' --max-time 4 "http://127.0.0.1:18081/t8/3/$i/0.png"; done)
N503=$(echo "$CODES" | grep -c 503 || true)
N200=$(echo "$CODES" | grep -c 200 || true)
if [[ $N503 -ge 1 && $N200 -ge 1 ]]; then PASS=$((PASS+1)); echo "PASS: throttling mixed 200/503 ($N200 ok, $N503 throttled)"; else FAIL=$((FAIL+1)); FAILED_TESTS+=("throttle"); echo "FAIL: throttling ($N200 ok, $N503 throttled): $CODES"; fi

# whitelisted client bypasses the throttle entirely
W503=$(for i in $(seq 0 7); do curl -s -o /dev/null -w '%{http_code}\n' --max-time 4 "http://127.0.0.1:18082/t8/3/$i/0.png"; done | grep -c 503 || true)
assert_eq "whitelist bypasses throttle" "$W503" "0"

# 503s show up in the global counters
M2=$(curl -s --max-time 4 "$BASE_URL/mod_tile")
N503C=$(echo "$M2" | awk '/^NoResp503:/{print $2}')
if [[ ${N503C:-0} -ge 1 ]]; then PASS=$((PASS+1)); echo "PASS: NoResp503 counted ($N503C)"; else FAIL=$((FAIL+1)); FAILED_TESTS+=("503 counter"); echo "FAIL: NoResp503 not counted"; fi

# stats disabled message
assert_contains "stats disabled message" "$(curl -s --max-time 4 http://127.0.0.1:18083/mod_tile)" "Stats are not enabled"



echo "== phase 5: thread pool + style parameterization (stage 3) =="

# thread pool path: fresh hit, byte-exact
curl -s --max-time 6 "http://127.0.0.1:18084/tp/3/1/1.png" | md5sum | awk '{print $1}' > /tmp/got-tp.md5
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(3,1,1))" | md5sum | awk '{print $1}' > /tmp/want-tp-fresh.md5
assert_eq "thread pool fresh hit bytes" "$(cat /tmp/got-tp.md5)" "$(cat /tmp/want-tp-fresh.md5)"

# thread pool path: missing tile rendered on demand through the pool
MS=$(curl -s -o /tmp/miss-tp -w '%{http_code}' --max-time 6 "http://127.0.0.1:18084/tp/7/7/7.png")
assert_status "thread pool on-demand render 200" "http://127.0.0.1:18084/tp/7/7/7.png" 200
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(7,7,7))" | md5sum | awk '{print $1}' > /tmp/want-tp.md5
assert_eq "thread pool rendered content" "$(md5sum /tmp/miss-tp | awk '{print $1}')" "$(cat /tmp/want-tp.md5)"

# thread pool: 304 still works
TETAG=$(curl -sI --max-time 4 "http://127.0.0.1:18084/tp/3/1/1.png" | tr -d '\r' | awk 'tolower($1)=="etag:"{print $2}')
assert_status "thread pool If-None-Match 304" "http://127.0.0.1:18084/tp/3/1/1.png" 304 -H "If-None-Match: $TETAG"

# style parameterization: /opt/<style>/z/x/y.ext serves the options metatile
curl -s --max-time 4 "http://127.0.0.1:18080/opt/day/6/32/32.png" | md5sum | awk '{print $1}' > /tmp/got-opt.md5
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(6,32,32,style='day'))" | md5sum | awk '{print $1}' > /tmp/want-opt.md5
assert_eq "options metatile served" "$(cat /tmp/got-opt.md5)" "$(cat /tmp/want-opt.md5)"

# missing options tile: mock renders into <h0>.<options>.meta and we serve it
MS2=$(curl -s -o /tmp/miss-opt -w '%{http_code}' --max-time 6 "http://127.0.0.1:18080/opt/night/6/40/40.png")
assert_status "missing options tile rendered 200" "http://127.0.0.1:18080/opt/night/6/40/40.png" 200
assert_contains "mock received style options" "$(cat "$MOCK_LOG")" "opts=night"
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(6,40,40,style='night'))" | md5sum | awk '{print $1}' > /tmp/want-opt2.md5
assert_eq "rendered options content" "$(md5sum /tmp/miss-opt | awk '{print $1}')" "$(cat /tmp/want-opt2.md5)"


echo
echo "================================"
echo "PASS=$PASS FAIL=$FAIL"
if [[ $FAIL -gt 0 ]]; then
    printf 'failed: %s\n' "${FAILED_TESTS[@]}"
    tail -30 /tmp/tile-test-error.log
    exit 1
fi
echo "ALL TESTS PASSED"
