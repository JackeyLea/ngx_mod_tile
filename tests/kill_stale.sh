#!/usr/bin/env bash
# Kill leftover test nginx/mock processes. Run as: tests/kill_stale.sh
# (kept in a separate script so pkill -f patterns never match the caller's
# own command line, which is how earlier runs silently killed themselves)
for pidfile in /tmp/tile-test-nginx.pid /tmp/tile-test-mock.pid; do
    if [[ -f "$pidfile" ]]; then
        kill "$(cat "$pidfile")" 2>/dev/null
        rm -f "$pidfile"
    fi
done

pkill -x nginx 2>/dev/null
pkill -f mock_renderd.py 2>/dev/null
sleep 0.5
exit 0
