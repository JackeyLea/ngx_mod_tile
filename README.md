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
- ✅ Per-worker cached storage backends (`file`, `null`; memcached/rados/etc. in stage 3)
- ⬜ Stage 2: `/mod_tile` stats page, `/metrics` (Prometheus), delaypool per-IP throttling, X-Forwarded-For
- ⬜ Stage 3: memcached / rados / composite / ro_http_proxy stores, style parameterization

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

    # endpoints handled inside every tile layer:
    #   /osm/z/x/y.png            tile
    #   /osm/z/x/y.png/status     freshness text
    #   /osm/z/x/y.png/dirty      submit cmdDirty, returns immediately
    #   /osm/tile-layer.json      TileJSON 2.0.0
}
```

Storage backend is chosen by the `tile_tile_dir` / renderd.conf `tiledir`
value: `/path` → file store, `null://` → blackhole. (`rados://`,
`memcached://`, `composite:{...}`, `ro_http_proxy://` are wired in stage 3.)

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
| Stats / throttling | built-in | stage 2 |
| renderd protocol | v2/v3 | v2/v3 — **drop-in compatible with an existing renderd and tile store** |

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
correct content, renderd-dead timeout paths, and the async-concurrency proof.

## License

- This module's glue code: GPL-2.0 (derivative of mod_tile; see `LICENSE`).
- Vendored code: mod_tile (GPL-2.0), iniparser (MIT, see `vendor/LICENSE`).
- nginx itself remains BSD-licensed; this module links dynamically.
