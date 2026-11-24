#!/usr/bin/env bash
# Run browser/user-interaction tests against a running ngx_mod_tile map stack.
#
# Usage:
#   tests/run_playwright_tests.sh                          # demo on :8090
#   tests/run_playwright_tests.sh http://127.0.0.1:8091/  # real OSM stack
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
IMAGE=ngx-mod-tile-playwright:latest
BASE_URL="${1:-http://127.0.0.1:8090/}"
ARTIFACT_DIR="${PLAYWRIGHT_ARTIFACT_DIR:-/tmp/ngx-mod-tile-playwright}"

CONTAINER_URL="${BASE_URL//127.0.0.1/host.docker.internal}"
CONTAINER_URL="${CONTAINER_URL//localhost/host.docker.internal}"

if ! curl -fsS -o /dev/null --max-time 5 "$BASE_URL"; then
    echo "Map stack is not reachable at $BASE_URL" >&2
    echo "Start it with: docker compose up -d" >&2
    exit 1
fi

echo "== building Playwright runner from the mirrored image =="
docker build -f "$REPO/docker/Dockerfile.playwright" -t "$IMAGE" "$REPO" \
    > /tmp/playwright-build.log

mkdir -p "$ARTIFACT_DIR"
echo "== running Chromium user-interaction tests against $BASE_URL =="
if ! docker run --rm \
    --add-host=host.docker.internal:host-gateway \
    --ipc=host \
    -e "MAP_TEST_TIMEOUT=${MAP_TEST_TIMEOUT:-90000}" \
    -v "$ARTIFACT_DIR:/artifacts" \
    "$IMAGE" "$CONTAINER_URL" /artifacts; then
    echo "Browser test failed; screenshot: $ARTIFACT_DIR/map-failure.png" >&2
    exit 1
fi

echo "Screenshot: $ARTIFACT_DIR/map.png"
