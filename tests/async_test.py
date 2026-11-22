#!/usr/bin/env python3
"""Asynchronous-state-machine proof for ngx_mod_tile (Scheme B).

While a burst of missing-tile requests is parked waiting on a slow renderd
(1.5 s simulated render), fresh-tile requests must keep completing in
milliseconds -- i.e. the worker event loop is not blocked by render waits.
"""
import concurrent.futures
import http.client
import sys
import threading
import time

BASE_HOST, BASE_PORT = "127.0.0.1", 18080
BURST = 200


def get(path, timeout=30):
    conn = http.client.HTTPConnection(BASE_HOST, BASE_PORT, timeout=timeout)
    try:
        conn.request("GET", path)
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        conn.close()


def main():
    base = sys.argv[1].rstrip("/") if len(sys.argv) > 1 else "http://127.0.0.1:18080"

    # sanity: the worker is up
    status, _ = get("/static/")
    assert status == 200, "nginx not reachable"

    stop = threading.Event()
    slowest_fresh = [0.0]
    fresh_failures = []

    def fresh_loop():
        """Hammer a fresh tile while render waits pile up."""
        n = 0
        while not stop.is_set():
            t0 = time.monotonic()
            status, body = get("/tiles/0/0/0.png", timeout=10)
            dt = time.monotonic() - t0
            if status != 200 or len(body) == 0:
                fresh_failures.append((status, dt))
                return
            if dt > slowest_fresh[0]:
                slowest_fresh[0] = dt
            n += 1
            time.sleep(0.01)

    watcher = threading.Thread(target=fresh_loop, daemon=True)
    watcher.start()

    # burst of 200 missing tiles, each parked ~1.5 s on the mock renderd
    t0 = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as pool:
        results = list(pool.map(
            lambda i: get("/tiles/7/%d/%d.png" % (i % 100, i % 100), timeout=30),
            range(BURST)))
    burst_time = time.monotonic() - t0
    stop.set()
    watcher.join(timeout=5)

    ok = sum(1 for s, _ in results if s == 200)
    print("burst: %d/%d rendered in %.2f s; slowest fresh request while "
          "waiting: %.3f s; fresh requests served meanwhile" % (
              ok, BURST, burst_time, slowest_fresh[0]))

    if fresh_failures:
        print("FAIL: fresh requests failed during burst: %r" % fresh_failures[:3])
        return 1
    if ok < BURST - 5:
        print("FAIL: only %d/%d burst requests rendered" % (ok, BURST))
        return 1
    if slowest_fresh[0] > 0.5:
        print("FAIL: worker appears blocked: fresh request took %.3f s"
              % slowest_fresh[0])
        return 1

    print("PASS: worker stayed responsive during %d concurrent render waits"
          % BURST)
    return 0


if __name__ == "__main__":
    sys.exit(main())
