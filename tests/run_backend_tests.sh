#!/usr/bin/env bash
# Network storage backend runtime integration tests for ngx_mod_tile (Docker).
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
COMPOSE_FILE="$REPO/docker-compose.backends.yml"
PROJECT_NAME="ngx_mod_tile_backends"
BASE_URL=http://127.0.0.1:8095
WITH_RADOS=0
if [[ "${1:-}" == "--rados" ]]; then
    WITH_RADOS=1
fi

PASS=0
FAIL=0
FAILED_TESTS=()

assert_eq() {
    local desc="$1" got="$2" want="$3"
    if [[ "$got" == "$want" ]]; then
        PASS=$((PASS+1)); echo "PASS: $desc"
    else
        FAIL=$((FAIL+1)); FAILED_TESTS+=("$desc")
        echo "FAIL: $desc (want $want, got $got)"
    fi
}

assert_contains() {
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

echo "== preparing backend test stack =="
if [[ $WITH_RADOS -eq 1 ]]; then
    docker compose -p "$PROJECT_NAME" -f "$COMPOSE_FILE" --profile rados up -d --build
    echo "== waiting for Ceph RADOS cluster =="
    for i in $(seq 1 90); do
        if docker compose -p "$PROJECT_NAME" -f "$COMPOSE_FILE" exec -T ceph \
            test -f /tmp/ceph-ready >/dev/null 2>&1; then
            break
        fi
        sleep 1
    done
else
    docker compose -p "$PROJECT_NAME" -f "$COMPOSE_FILE" up -d --build
fi

echo "== waiting for tileserver =="
for i in $(seq 1 60); do
    if curl -s -o /dev/null --max-time 2 "$BASE_URL/healthz"; then
        break
    fi
    sleep 1
done

echo "== seeding memcached with metatiles =="
docker compose -p "$PROJECT_NAME" -f "$COMPOSE_FILE" exec -T tileserver \
    python3 /opt/tests/seed_memcached.py memcached 11211

# ------------------------------------------------------------------
# Memcached backend
# ------------------------------------------------------------------
echo "== testing memcached backend =="

# 1. Fresh tile hit via memcached (thread pool)
assert_eq "memcached fresh tile 200" \
    "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE_URL/mc/0/0/0.png")" "200"

# 2. Byte-exact content verification
curl -s --max-time 5 "$BASE_URL/mc/3/5/5.png" | md5sum | awk '{print $1}' > /tmp/mc_got.md5
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(3,5,5))" | md5sum | awk '{print $1}' > /tmp/mc_want.md5
assert_eq "memcached tile content byte-exact" "$(cat /tmp/mc_got.md5)" "$(cat /tmp/mc_want.md5)"

# 3. Response headers
MC_HEADERS=$(curl -sI --max-time 5 "$BASE_URL/mc/3/5/5.png" | tr -d '\r')
assert_contains "memcached Content-Type" "$MC_HEADERS" "Content-Type: image/png"
assert_contains "memcached ETag" "$MC_HEADERS" "ETag:"
assert_contains "memcached Cache-Control" "$MC_HEADERS" "max-age="

# 4. 304 Not Modified via ETag
ETAG=$(echo "$MC_HEADERS" | awk 'tolower($1)=="etag:"{print $2}')
assert_eq "memcached If-None-Match -> 304" \
    "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -H "If-None-Match: $ETAG" "$BASE_URL/mc/3/5/5.png")" "304"

# 5. Direct event-loop path (without thread pool)
assert_eq "memcached direct (no thread pool) 200" \
    "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE_URL/mc-sync/1/1/1.png")" "200"

# 6. Missing tile triggers on-demand render
assert_eq "memcached missing tile on-demand render 200" \
    "$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$BASE_URL/mc/7/5/5.png")" "200"

# ------------------------------------------------------------------
# Read-only HTTP proxy backend
# ------------------------------------------------------------------
echo "== testing ro_http_proxy backend =="

assert_eq "http proxy tile 200" \
    "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE_URL/http/2/2/2.png")" "200"

curl -s --max-time 5 "$BASE_URL/http/2/2/2.png" | md5sum | awk '{print $1}' > /tmp/http_got.md5
python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(2,2,2))" | md5sum | awk '{print $1}' > /tmp/http_want.md5
assert_eq "http proxy tile content byte-exact" "$(cat /tmp/http_got.md5)" "$(cat /tmp/http_want.md5)"

# ------------------------------------------------------------------
# Ceph RADOS backend (optional, --rados)
# ------------------------------------------------------------------
if [[ $WITH_RADOS -eq 1 ]]; then
    echo "== seeding RADOS pool =="
    docker compose -p "$PROJECT_NAME" -f "$COMPOSE_FILE" exec -T ceph \
        python3 /opt/tests/seed_rados.py

    echo "== testing rados backend =="
    assert_eq "rados fresh tile 200" \
        "$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$BASE_URL/rados/0/0/0.png")" "200"

    curl -s --max-time 8 "$BASE_URL/rados/2/2/2.png" | md5sum | awk '{print $1}' > /tmp/rados_got.md5
    python3 -c "import sys; sys.path.insert(0,'$HERE'); from gen_metatiles import tile_png; sys.stdout.buffer.write(tile_png(2,2,2))" | md5sum | awk '{print $1}' > /tmp/rados_want.md5
    assert_eq "rados tile content byte-exact" "$(cat /tmp/rados_got.md5)" "$(cat /tmp/rados_want.md5)"

    RADOS_HEADERS=$(curl -sI --max-time 8 "$BASE_URL/rados/2/2/2.png" | tr -d '\r')
    assert_contains "rados Content-Type" "$RADOS_HEADERS" "Content-Type: image/png"
    RADOS_ETAG=$(echo "$RADOS_HEADERS" | awk 'tolower($1)=="etag:"{print $2}')
    assert_eq "rados If-None-Match -> 304" \
        "$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -H "If-None-Match: $RADOS_ETAG" "$BASE_URL/rados/2/2/2.png")" "304"
fi

# ------------------------------------------------------------------
# Stats and metrics validation
# ------------------------------------------------------------------
echo "== validating stats counters =="
M=$(curl -s --max-time 5 "$BASE_URL/mod_tile")
assert_contains "stats NoResp200 counted" "$M" "NoResp200: "
assert_contains "stats layer counter" "$M" "NoRes200Layer/mc/"
if [[ $WITH_RADOS -eq 1 ]]; then
    assert_contains "stats rados layer counter" "$M" "NoRes200Layer/rados/"
fi

echo
echo "================================"
echo "PASS=$PASS FAIL=$FAIL"
if [[ $FAIL -gt 0 ]]; then
    printf 'failed: %s\n' "${FAILED_TESTS[@]}"
    echo "== tileserver error logs =="
    docker compose -p "$PROJECT_NAME" -f "$COMPOSE_FILE" logs --tail=30 tileserver
    exit 1
fi
echo "ALL BACKEND INTEGRATION TESTS PASSED"
