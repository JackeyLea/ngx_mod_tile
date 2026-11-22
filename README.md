# ngx_mod_tile

An nginx port of OpenStreetMap's [mod_tile](https://github.com/openstreetmap/mod_tile):
serves slippy-map tiles from mod_tile-compatible metatile stores and submits
on-demand render requests to `renderd` — without Apache.

Renders map tiles with [Mapnik](https://mapnik.org/) (via renderd) and serves
them using nginx.

## Status: Phase 1 (MVP) — working

- ✅ mod_tile-compatible metatile serving (file store, `META` and gzip `METZ`)
- ✅ renderd v2/v3 wire protocol, **fully asynchronous** (see Architecture)
- ✅ On-demand rendering with priorities (missing→RenderPrio, old→RenderLow, veryold→Render)
- ✅ Load-based shedding (`tile_max_load_old` / `tile_max_load_missing`)
- ✅ ETag (MD5) / `If-None-Match` 304 / `Cache-Control` + `Expires` heuristic (port of `add_expiry`, incl. jitter)
- ✅ CORS with `OPTIONS` preflight short-circuit (port of `add_cors`)
- ✅ `tile-layer.json` (TileJSON 2.0.0), `/status`, `/dirty` endpoints
- ✅ `tile_import_config` — reads `renderd.conf` `[map]` sections (iniparser)
- ✅ **Stage 2**: shared-memory stats counters, `/mod_tile` stats page, `/metrics` (Prometheus), per-IP delaypool throttling with whitelist and X-Forwarded-For
- ✅ **Stage 3**: `tile_thread_pool` — tile_stat/tile_read offloaded to nginx thread pools (per-thread backend instances via pthread TLS), making the blocking network stores safe to use; style parameterization (`tile_options on`, options passed through in the v3 packet, options-aware metatile paths `<h0>.<options>.meta`)
- ✅ Storage backends vendored with build-time switches: `file`/`null` (always), `memcached`/`rados`/`ro_http_proxy`/`composite` via `NGX_TILE_WITH_*` configure env vars

## Architecture

Apache mod_tile relies on per-request threads/processes and blocks freely
(`poll()` waiting up to `request_timeout` seconds for renderd, `sleep(3)`
penalties). nginx workers are single-threaded and event-driven, where any
block stalls every connection. This port therefore implements **Scheme B
(fully async)**:

```
preaccess phase: ngx_http_tile_matcher
  URI → layer (location tile_layer or tile_import_config) → parse z/x/y
  → r->content_handler = ngx_http_tile_content   (beats the static handler,
                                                   access-phase still applies)

content phase: ngx_http_tile_content
  tile_stat → fresh ? serve
            → old/veryold/missing ? submit render and SUSPEND the request
                (r->main->count++, return NGX_DONE — zero threads, zero polling)

async renderd engine (tile_renderd.c), one state machine per exchange:
  ngx_event_connect_peer (non-blocking connect, unix socket or TCP)
  → send struct protocol (v2 61 B / v3 143 B, EAGAIN resumes on write event)
  → accumulate reply, discover version from the first int, deadline timer
  → cmdDone → re-stat → serve; notDone/timeout → stale or 404
  fire-and-forget cmdDirty runs detached in its own pool and outlives the request
```

While hundreds of render waits are parked, the event loop keeps serving other
requests — the test suite proves fresh tiles answer in ≤1 ms during a
200-request render burst (`tests/async_test.py`).

Reused verbatim from upstream (vendored under `vendor/`): `store.c`,
`store_file*.c`, `store_null.c`, `metatile.h`, `protocol.h`, `render_config.h`,
`sys_utils.c`, plus `store_file_utils.c`. A small shim (`g_logger_ngx.c`)
replaces the glib logging dependency; `iniparser` (MIT) parses renderd.conf.
No C++ is needed on the serving path (`metatile.cpp` is renderd-only).

## Building

Builds as an nginx dynamic module (`--with-compat`), so the `.so` also loads
into any compat-enabled nginx of the same major version:

```sh
# fetch nginx sources once
cd build
curl -sSLO https://nginx.org/download/nginx-1.30.7.tar.gz
tar xzf nginx-1.30.7.tar.gz && mv nginx-1.30.7 nginx-src

cd nginx-src
./configure --prefix=$PWD/../nginx --with-compat \
            --add-dynamic-module=/path/to/ngx_mod_tile
make modules && make

# load_module /path/to/ngx_http_tile_module.so;
```

Without libpcre2/libssl-dev, add `--without-http_rewrite_module
--without-http_ssl_module` (the tile module itself has no extra dependencies —
no glib, no iniparser system package, no C++ runtime).

## Configuration

Directives live at http/server scope (settings) and location scope (layers).

```nginx
load_module modules/ngx_http_tile_module.so;

server {
    listen 80;
    server_name tile.example.com;

    tile_renderd_socket_addr unix:/run/renderd/renderd.sock;  # or host:port
    tile_tile_dir /var/cache/renderd/tiles;
    tile_request_timeout 3s;            # wait for renderd (low/normal prio)
    tile_missing_request_timeout 3s;    # wait for missing-tile renders
    tile_renderd_connect_timeout 1s;
    tile_max_load_old 16;               # 1-min loadavg thresholds
    tile_max_load_missing 50;
    tile_very_old_threshold 365d;

    tile_cache_duration_minimum 3h;     # per-zoom Cache-Control heuristic
    tile_cache_duration_max 7d;
    tile_cache_duration_low_zoom 0 6d;
    tile_cache_duration_medium_zoom 0 1d;
    tile_cache_duration_dirty 15m;
    tile_cache_last_modified_factor 0.0;
    tile_cache_extended_hostname cache.example.com;  # skips the max clamp
    tile_cache_extended_duration 24h;

    # Option A: one location per layer
    location /osm/ {
        tile_layer default;
        tile_file_extension png;
        tile_mime_type image/png;
        tile_minzoom 0;
        tile_maxzoom 20;
        tile_cors http://allowed.example.com;   # or "*" ; empty = off
        tile_hostname http://tile.example.com;  # TileJSON URL prefix (repeatable)
    }

    # Option B: import [map] sections from renderd.conf (uri prefix routing)
    tile_import_config /etc/renderd.conf;

    # stage 2: statistics (counters live in a shared zone, global across servers)
    tile_stats_zone 4m;                  # default 4m
    location = /mod_tile { tile_stats on; }
    location = /metrics { tile_metrics on; }

    # stage 2: per-IP throttling (token buckets in shared memory)
    tile_throttling_zone 8m;             # defines the pool; required for throttling
    tile_throttling on;                  # per server/scope
    tile_throttling_tiles 5000 1;        # bucket size, refill per second
    tile_throttling_renders 65 0.2;
    tile_throttling_whitelist 10.0.0.5;  # exempt, up to 13 IPv4 entries
    tile_throttling_xforward 0;          # 1 = first XFF entry, 2 = last

    # stage 3: run blocking store I/O in a thread pool (nginx --with-threads;
    # define the pool with nginx's own "thread_pool" directive first)
    thread_pool default threads=4;
    location /tiles/ {
        tile_layer default;
        tile_thread_pool default;
    }

    # stage 3: style parameterization URLs (options passed to renderd in v3)
    location /osm/ {
        tile_layer default;
        tile_options on;                 # /osm/<style>/z/x/y.png[/status|/dirty]
    }

    # endpoints handled inside every tile layer:
    #   /osm/z/x/y.png            tile
    #   /osm/z/x/y.png/status     freshness text
    #   /osm/z/x/y.png/dirty      submit cmdDirty, returns immediately
    #   /osm/tile-layer.json      TileJSON 2.0.0
}
```

Storage backend is chosen by the `tile_tile_dir` / renderd.conf `tiledir`
value: `/path` → file store, `null://` → blackhole, `memcached://`,
`rados://`, `composite:{...}`, `ro_http_proxy://`. The network-backed stores
are vendored verbatim but need their dev libraries at build time — enable
them by exporting at configure:

```sh
NGX_TILE_WITH_MEMCACHED=yes   # -lmemcached
NGX_TILE_WITH_RADOS=yes       # -lrados
NGX_TILE_WITH_RO_HTTP_PROXY=yes  # -lcurl
NGX_TILE_WITH_COMPOSITE=yes   # -lcairo (PNG blending)

./configure --with-compat --with-threads --add-dynamic-module=/path/to/ngx_mod_tile ...
```

Backends without `tile_thread_pool` run blocking I/O in the event loop (fine
for the file store, not for network stores) — the module logs a warning at
startup when a network URI is configured without a pool.

## Semantics vs Apache mod_tile

| Aspect | Apache mod_tile | ngx_mod_tile |
|---|---|---|
| Render wait | blocking `poll()` per request thread | async event state machine, request suspended |
| Invalid/out-of-bounds requests | `sleep(3)` penalty then 404 | immediate 404 (throttle with nginx `limit_req`) |
| Renderd socket connect | blocking | non-blocking + `tile_renderd_connect_timeout` |
| Layer routing | first-prefix-match over global list | nginx locations; or `tile_import_config` prefix list |
| Conditional requests | full `ap_meets_conditions` | `If-None-Match` only (mod_tile never sends Last-Modified) |
| TileJSON hostnames | `ServerName`/`ServerAlias` | `tile_hostname` (repeatable) or `server_name` |
| JSON escaping | none | none (byte-compatible) |
| Stats / throttling | built-in, global fill-up pass over 100k slots | stage 2: shm zones, per-slot lazy refill, immediate 503 (no `sleep(3)`), config-based whitelist |
| renderd protocol | v2/v3 | v2/v3 — **drop-in compatible with an existing renderd and tile store** |

## Docker

One-command build + run (multi-stage image: nginx + module compiled with
every optional store; demo tile store generated on first start):

```sh
docker compose up -d --build
```

The stack mirrors the real architecture — nginx talks to a renderer over
TCP and reads tiles from a shared volume:

| service | role |
|---|---|
| `tileserver` | nginx + `ngx_http_tile_module` (all storage backends compiled) |
| `renderd` | demo renderer speaking the real mod_tile protocol on `renderd:7654`, writes metatiles to the `tiles` volume |

Try it:

```sh
curl http://localhost:8090/tiles/3/1/1.png            # fresh hit from the demo store
curl http://localhost:8090/tiles/7/9/9.png            # miss -> demo renderd renders -> 200
curl http://localhost:8090/styles/day/6/32/32.png     # style parameterization
curl http://localhost:8090/mod_tile                   # text counters
curl http://localhost:8090/metrics                    # Prometheus
```

The renderer in the image is a **demo** placeholder (no Mapnik) — for real
deployments point `tile_renderd_socket_addr` at an actual renderd and mount
its tile directory; protocol and store layout are identical. Build knobs:
the Dockerfile exports all `NGX_TILE_WITH_*` switches; `DEMO_RENDERD=external`
on the tileserver disables its embedded renderer (compose sets this).

## Testing

```sh
tests/run_tests.sh
```

Spins up a mock renderd (`tests/mock_renderd.py`, speaks the real v2/v3
protocol over a unix socket) and nginx with a generated metatile store
(`tests/gen_metatiles.py`, byte-layout verified against upstream), then asserts
52 behaviors: fresh hits and byte-exact bodies, HEAD, ETag/304, CORS
(echo/Vary/403/preflight), gzip METZ handling, oob/bad-extension 404s, the
renderd.conf-imported layer, TileJSON fields, /status texts, /dirty wire
traffic, stale-tile serving with render submission, on-demand rendering with
correct content, renderd-dead timeout paths, the async-concurrency proof, plus
the stage 2 counters (/mod_tile fields, Prometheus families and labels), the
per-IP throttle 503s with whitelist bypass, the stats-disabled message, the
thread-pool serving path (fresh/304/on-demand render), and style
parameterization including options-aware render round-trips.

## License

- This module's glue code: GPL-2.0 (derivative of mod_tile; see `LICENSE`).
- Vendored code: mod_tile (GPL-2.0), iniparser (MIT, see `vendor/LICENSE`).
- nginx itself remains BSD-licensed; this module links dynamically.
