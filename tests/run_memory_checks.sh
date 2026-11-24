#!/usr/bin/env bash
# Run AddressSanitizer (ASan) and Valgrind memory checks for ngx_mod_tile.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
ASAN_DIR="$REPO/build/asan"
ASAN_NGINX="$ASAN_DIR/objs/nginx"
ASAN_MODULE="$ASAN_DIR/objs/ngx_http_tile_module.so"

echo "===================================================="
echo " ngx_mod_tile memory checks"
echo "===================================================="

"$HERE/kill_stale.sh"

# ------------------------------------------------------------------
# 1. ASan instrumented build & test run
# ------------------------------------------------------------------
if [[ ! -x "$ASAN_NGINX" || ! -f "$ASAN_MODULE" ]]; then
    echo "ASan binaries not found. Building ASan-instrumented nginx..."
    mkdir -p "$ASAN_DIR"
    if [[ ! -f "$ASAN_DIR/configure" ]]; then
        tar -xf "$REPO/build/nginx-1.30.7.tar.gz" -C "$ASAN_DIR" --strip-components=1
    fi
    (
        cd "$ASAN_DIR" || exit 1
        ./configure \
            --prefix="$ASAN_DIR/nginx" \
            --with-compat \
            --with-threads \
            --without-http_rewrite_module \
            --with-cc-opt="-g -O1 -fsanitize=address -fno-omit-frame-pointer -Wno-error" \
            --with-ld-opt="-fsanitize=address" \
            --add-dynamic-module="$REPO" > /tmp/asan_configure.log
    )
fi

echo "Building ASan-instrumented module..."
( cd "$ASAN_DIR" && make -j"$(nproc)" modules > /tmp/asan_build.log ) || {
    echo "ASan build failed. See /tmp/asan_build.log"
    exit 1
}

echo
echo "== Running full test suite with AddressSanitizer =="
export TILE_TEST_NGINX="$ASAN_NGINX"
export TILE_TEST_MODULE="$ASAN_MODULE"
export ASAN_OPTIONS="detect_odr_violation=0:detect_leaks=1:abort_on_error=1:log_path=/tmp/asan_tile"
export LSAN_OPTIONS="suppressions=$HERE/lsan.supp"
rm -f /tmp/asan_tile.* || true

sleep 0.3
"$HERE/run_tests.sh" > /tmp/testrun_asan.log 2>&1
ASAN_TEST_EXIT=$?

tail -n 5 /tmp/testrun_asan.log

# Check if ASan/LSan emitted any reports (even after cleanup)
"$HERE/kill_stale.sh"
sleep 0.3

ASAN_ERRORS=0
if ls /tmp/asan_tile.* >/dev/null 2>&1; then
    for report in /tmp/asan_tile.*; do
        # Only treat reports containing actual sanitizer errors as failures.
        # LSan may write a file containing only the suppression summary.
        if grep -E "ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:" "$report" >/dev/null; then
            echo "WARNING: ASan reported errors in $report:"
            head -n 20 "$report"
            ASAN_ERRORS=1
        fi
    done
fi

if [[ $ASAN_ERRORS -eq 0 ]]; then
    echo "ASan check: no memory errors or unsuppressed leaks detected."
fi

unset TILE_TEST_NGINX TILE_TEST_MODULE ASAN_OPTIONS LSAN_OPTIONS

# ------------------------------------------------------------------
# 2. Valgrind memcheck run
# ------------------------------------------------------------------
if command -v valgrind >/dev/null 2>&1; then
    echo
    echo "== Running Valgrind memcheck =="
    sleep 0.3
    "$HERE/run_valgrind.sh" > /tmp/testrun_valgrind.log 2>&1
    VG_EXIT=$?
    tail -n 8 /tmp/testrun_valgrind.log
else
    echo "valgrind not found; skipping memcheck run."
    VG_EXIT=0
fi

echo
echo "===================================================="
if [[ $ASAN_TEST_EXIT -eq 0 && $ASAN_ERRORS -eq 0 && $VG_EXIT -eq 0 ]]; then
    echo "ALL MEMORY CHECKS PASSED"
    exit 0
else
    echo "MEMORY CHECKS FAILED (ASan tests=$ASAN_TEST_EXIT, ASan reports=$ASAN_ERRORS, Valgrind=$VG_EXIT)"
    exit 1
fi
