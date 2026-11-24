#!/usr/bin/env bash
# Valgrind memcheck runner for ngx_mod_tile.
#
# Starts nginx in single-process mode under valgrind, drives tile requests
# across all serving paths, then exits gracefully to check for leaks.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
NGINX="$REPO/build/nginx-src/objs/nginx"
NGINX_CONF="$HERE/nginx_valgrind.conf"
BASE=/tmp/tilestore-valgrind
SOCKET=/tmp/tile-valgrind-renderd.sock
MOCK_LOG=/tmp/mock-renderd-valgrind.log
VG_LOG=/tmp/valgrind-tile.log

echo "== preparing tile store =="
rm -rf "$BASE" "$MOCK_LOG" "$VG_LOG"
mkdir -p /tmp/tile-test-body /tmp/tile-test-proxy /tmp/tile-test-fastcgi /tmp/tile-test-uwsgi /tmp/tile-test-scgi
python3 "$HERE/gen_metatiles.py" "$BASE" > /dev/null

rm -f "$SOCKET"
python3 "$HERE/mock_renderd.py" --socket "$SOCKET" --log "$MOCK_LOG" --mode done --delay 0.05 --render-dir "$BASE" &
MOCK_PID=$!
for i in $(seq 1 50); do [[ -S "$SOCKET" ]] && break; sleep 0.05; done

echo "== starting nginx under valgrind =="
valgrind \
    --tool=memcheck \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite \
    --track-origins=yes \
    --error-exitcode=99 \
    --num-callers=20 \
    --suppressions="$HERE/nginx_valgrind.supp" \
    --log-file="$VG_LOG" \
    "$NGINX" -e /tmp/tile-valgrind-error.log -c "$NGINX_CONF" &
NGINX_PID=$!

cleanup() {
    kill "$NGINX_PID" 2>/dev/null
    kill "$MOCK_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

echo "== waiting for nginx =="
for i in $(seq 1 150); do
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:18100/tiles/0/0/0.png"; then
        break
    fi
    if ! kill -0 "$NGINX_PID" 2>/dev/null; then
        echo "FATAL: nginx under valgrind exited early"
        cat "$VG_LOG" || true
        exit 1
    fi
    sleep 0.2
done

URL=http://127.0.0.1:18100
echo "== driving tile requests =="

# Fresh hits & ETag/304
curl -s -o /dev/null "$URL/tiles/0/0/0.png"
curl -s -o /dev/null "$URL/tiles/3/5/5.png"
ETAG=$(curl -sI "$URL/tiles/3/5/5.png" | tr -d '\r' | awk 'tolower($1)=="etag:"{print $2}')
curl -s -o /dev/null -H "If-None-Match: $ETAG" "$URL/tiles/3/5/5.png"

# CORS
curl -s -o /dev/null -H "Origin: http://allowed.example.com" "$URL/tiles/3/5/5.png"
curl -s -o /dev/null -H "Origin: http://evil.example.com" "$URL/tiles/3/5/5.png"
curl -s -o /dev/null -X OPTIONS -H "Origin: http://allowed.example.com" -H "Access-Control-Request-Method: GET" "$URL/tiles/3/5/5.png"

# METZ
curl -s -o /dev/null "$URL/tiles/7/64/64.png"
curl -s -o /dev/null -H 'Accept-Encoding: gzip' "$URL/tiles/7/64/64.png"

# On-demand rendering
curl -s -o /dev/null "$URL/tiles/7/5/5.png"

# Old tiles
touch -d "100 days ago" /tmp/tilestore-valgrind/test/8/0/0/0/68/0.meta
curl -s -o /dev/null "$URL/tiles/8/64/64.png"
curl -s -o /dev/null "$URL/tiles/9/64/64.png"

# Status and Dirty
curl -s -o /dev/null "$URL/tiles/3/5/5.png/status"
curl -s -o /dev/null "$URL/tiles/7/100/100.png/dirty"

# Thread pool paths
curl -s -o /dev/null "$URL/tp/3/1/1.png"
curl -s -o /dev/null "$URL/tp/7/7/7.png"

# Options mode
curl -s -o /dev/null "$URL/opt/day/6/32/32.png"
curl -s -o /dev/null "$URL/opt/night/6/40/40.png"
curl -s -o /dev/null "$URL/opt/night/6/40/40.png/dirty"

# Stats and Metrics
curl -s -o /dev/null "$URL/mod_tile"
curl -s -o /dev/null "$URL/metrics"

# Coordinate errors
curl -s -o /dev/null "$URL/tiles/abc/1/1.png"
curl -s -o /dev/null "$URL/tiles/30/0/0.png"
curl -s -o /dev/null "$URL/tiles/3/9999/0.png"

sleep 0.5

echo "== shutting down gracefully =="
kill -QUIT "$NGINX_PID"
for i in $(seq 1 100); do
    kill -0 "$NGINX_PID" 2>/dev/null || break
    sleep 0.2
done

wait "$NGINX_PID" 2>/dev/null
VG_EXIT=$?

echo "== valgrind summary =="
tail -n 20 "$VG_LOG"

if grep -E "Invalid read|Invalid write|use of uninitialised|Source and destination overlap|mismatched free" "$VG_LOG"; then
    echo "FAILED: valgrind detected memory access errors"
    exit 1
fi

if grep -q "definitely lost: 0 bytes in 0 blocks" "$VG_LOG"; then
    echo "PASSED: no definite memory leaks"
else
    echo "FAILED: definite memory leaks detected"
    grep -A 10 "definitely lost" "$VG_LOG" || true
    exit 1
fi

exit "$VG_EXIT"
