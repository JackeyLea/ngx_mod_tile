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

## Map data directory (`maps/`)

`maps/` holds OSM extracts for the real rendering stack:

```sh
./maps/download-demo-map.sh          # Baku ~12 MB (BBBike; Geofabrik is unreachable from CN)
# or any Geofabrik region:
OSM_PBF_URL="https://download.geofabrik.de/asia/azerbaijan-latest.osm.pbf" \
    ./maps/download-demo-map.sh azerbaijan
```

`*.pbf` files are git-ignored; the offline demo stack does not need them
(it uses the generated placeholder store), but the real rendering stack
(profile `osm`) imports whatever `.pbf` is in `maps/`.

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

The default renderer is a **demo** placeholder (no Mapnik). The real stack —
the one from the offline map server guide — lives behind the `osm` profile:

```sh
./maps/download-demo-map.sh                       # maps/Baku.osm.pbf (~12 MB)
docker compose --profile osm up -d --build
docker compose logs -f import                     # osm2pgsql import (~1 min for Baku)
# open http://localhost:8091/                     # Leaflet frontend
# tiles render on demand: curl http://localhost:8091/tiles/12/2521/1341.png
```

| service (profile osm) | role |
|---|---|
| `db` | PostGIS 16 (docker.m.daocloud.io mirror, override `REGISTRY_MIRROR` in `.env`) |
| `import` | one-shot osm2pgsql `--slim --hstore` + carto lua transform + `indexes.sql`/`functions.sql` + external land/water data |
| `osm-renderd` | renderd + Mapnik + openstreetmap-carto v5.9.0 (compiled with carto from the CN npm mirror) |
| `osm-tileserver` | ngx_mod_tile, layers parsed from renderd.conf via `tile_import_config`, Leaflet page at `/` |

Verified end-to-end on Ubuntu 24.04: the import finishes in seconds for the
Baku extract, and the rendered tiles show the full openstreetmap-carto style
(road network, water bodies, place labels). For the fast path without the
external land/ocean downloads (~600 MB), use `IMPORT_EXTERNAL=skip docker
compose --profile osm up -d` — the corresponding layers are then omitted from
the style (ocean areas show the map background colour instead).

The stack renders a real map on the first request (Baku centre at zoom 12 is
a 31 KB PNG). Restart `osm-renderd` after finishing the external data import
to restore the land/ocean layers if you previously used the skip mode.

### Ubuntu 26.04 variant

Both Dockerfiles accept `UBUNTU_VERSION` (default `24.04`). The supplied
`docker-compose.ubuntu26.yml` builds the same demo and real OSM services on
Ubuntu 26.04, with separate image tags, ports and persistent volumes:

```sh
# demo map on http://localhost:8096/
docker compose -p ngx_mod_tile_u26 \
    -f docker-compose.yml -f docker-compose.ubuntu26.yml \
    up -d --build renderd tileserver

# real OSM map on http://localhost:8097/ (PBF in maps/ first)
docker compose -p ngx_mod_tile_u26 \
    -f docker-compose.yml -f docker-compose.ubuntu26.yml \
    up -d --build db import osm-renderd osm-tileserver

# watch the one-shot import
docker compose -p ngx_mod_tile_u26 \
    -f docker-compose.yml -f docker-compose.ubuntu26.yml logs -f import
```

Images are tagged `ngx-mod-tile:ubuntu26.04` and
`ngx-mod-tile-renderd:ubuntu26.04`. The project name `ngx_mod_tile_u26` keeps
containers/volumes separate from the 24.04 stack, so both versions can run
side by side. The real stack starts renderd only after the PBF import and uses
its own `tiles-osm` volume with `GENERATE_DEMO_STORE=no`, preventing demo
placeholder tiles from shadowing real renders. No Nominatim or OSRM services
are included.

Verified with Ubuntu 26.04.1 LTS, osm2pgsql 2.2.0 and Mapnik 4.2.1: the Baku
import and real 256x256 rendering work, and both the demo (8096) and real map
(8097) pass all 13 Playwright user-interaction assertions. `fonts-noto-core`
is installed explicitly because the style's optional remote font download is
not always reachable.

All source fetches default to CN mirrors: apt → `mirrors.cloud.tencent.com`,
npm/cartotool → `registry.npmmirror.com`, Leaflet → npmmirror tarball,
PostGIS image → `docker.m.daocloud.io`. The openstreetmap-carto repo is
cloned on the HOST by `docker/prepare-renderd.sh` (github is not reachable
from inside the container); override `GH_PROXY` if needed. Map data source
default is BBBike because download.geofabrik.de is blocked here. Build knobs:
the Dockerfile exports all `NGX_TILE_WITH_*` switches; `DEMO_RENDERD=external`
on the tileserver disables its embedded renderer (compose sets this).

## Testing

```sh
tests/run_tests.sh
```

Spins up a mock renderd (`tests/mock_renderd.py`, speaks the real v2/v3
protocol over a unix socket) and nginx with a generated metatile store
(`tests/gen_metatiles.py`, byte-layout verified against upstream), then asserts
146 behaviors across 11 phases: fresh hits and byte-exact bodies, HEAD, ETag/304, CORS
(echo/Vary/403/preflight), gzip METZ handling, oob/bad-extension 404s, the
renderd.conf-imported layer, TileJSON fields, /status texts, /dirty wire
traffic, stale-tile serving with render submission, on-demand rendering with
correct content, renderd-dead timeout paths, the async-concurrency proof, plus
the stage 2 counters (/mod_tile fields, Prometheus families and labels), the
per-IP throttle 503s with whitelist bypass and X-Forwarded-For modes, the
stats-disabled message, the thread-pool serving path (fresh/304/on-demand
render), style parameterization including options-aware render round-trips,
plus the coverage gaps: bulk mode (RenderBulk submission, stale old tiles),
load shedding (dirty submission, missing 404), render-bucket exhaustion
(503s, single render submitted), XFF modes 1/2 (first vs last entry),
cmdNotDone replies, disabled status/dirty switches, the full Cache-Control
algorithm (dirty-branch jitter bounds, low/medium/default zoom tiers,
extended-hostname exact max-age, Expires/max-age consistency,
last_modified_factor clamping), coordinate/URI edge cases (non-numeric, negative, overflow, empty segments,
missing/wrong/uppercase extensions, zoom and coordinate bounds, extra
segments and trailing slashes), options-mode status/dirty suffixes
(clean/missing status text, cmdDirty submission and confirmation, coordinate
bounds, bad suffix, extra segments), multi-worker consistency (worker_processes 2,
shared-memory token bucket exhaustion across workers, zone counter aggregation),
and stats zone retention across reload (HUP signal preserves NoResp200, layer, zoom,
Prometheus metrics, and subsequent requests continue incrementing preserved counters).

### Memory checks (ASan & Valgrind)

```sh
tests/run_memory_checks.sh
```

This script builds an AddressSanitizer-instrumented version of nginx and the
dynamic module in `build/asan/`, runs the full 146-assertion test suite with
LeakSanitizer enabled (`ASAN_OPTIONS` / `LSAN_OPTIONS` with `tests/lsan.supp`),
and then runs a single-process Valgrind memcheck session (`tests/run_valgrind.sh`
with `tests/nginx_valgrind.supp`).

Running these checks uncovered and fixed two real memory safety bugs:
1. **Heap-use-after-free in `src/tile_renderd.c`**: `ngx_http_tile_conn_finish`
   accessed the connection context (`tc->pool`) after invoking the completion
   callback, which finalizes the HTTP request and destroys the request pool
   where `tc` resides. Fixed by caching the detached pool pointer before
   invoking the callback.
2. **Stack-buffer-overflow in `src/tile_stats.c`**: the stats/metrics endpoints
   copied the shared-memory stats struct plus a dynamically-sized layer counter
   array into a fixed-size stack variable. Fixed by allocating the snapshot
   buffer dynamically from the request pool based on `mcf->layer_count`.

Both checks now pass cleanly with zero memory errors and zero unsuppressed
definite leaks.

### Network backend integration (Docker)

```sh
# memcached + read-only HTTP proxy: 12 assertions
tests/run_backend_tests.sh

# additionally start a single-node Ceph cluster and test RADOS: 17 assertions
tests/run_backend_tests.sh --rados
```

`docker-compose.backends.yml` starts memcached, a deterministic HTTP tile
upstream, a memcached-aware test renderd, and nginx on port 8095. The suite
verifies byte-exact metatile reads, ETag/304, Cache-Control, thread-pool and
direct memcached paths, an on-demand render/write/read round trip, HTTP proxy
fetching, and layer counters. With `--rados`, a minimal Ceph v19 MON/MGR/OSD
cluster (official `quay.io/ceph/ceph:v19` image) creates the `tiles` pool and
verifies librados reads and conditional requests through the module.

### Browser/user-interaction tests (Playwright)

```sh
# demo map on http://localhost:8090/
tests/run_playwright_tests.sh

# real OSM/Mapnik map on http://localhost:8091/
tests/run_playwright_tests.sh http://127.0.0.1:8091/
```

`docker/Dockerfile.playwright` extends the locally mirrored
`mcr.m.daocloud.io/playwright:v1.49.1-noble` image and installs only the
matching Node package (the Chromium/Firefox/WebKit binaries are already in the
base image). The Chromium suite performs 13 assertions covering page/asset
loads, decoded PNG tiles, zoom-in/zoom-out button clicks, mouse dragging, tile
HTTP statuses, and JavaScript/console errors. Screenshots are written to
`/tmp/ngx-mod-tile-playwright/map.png`.

The browser suite exposed a real frontend bug: nginx returned `leaflet.css` as
`text/plain`, so Chromium refused to apply it. Both Docker nginx configurations
now include `mime.types`; the test also asserts browser-accepted JavaScript and
CSS MIME types to prevent regressions.

## License

- This module's glue code: GPL-2.0 (derivative of mod_tile; see `LICENSE`).
- Vendored code: mod_tile (GPL-2.0), iniparser (MIT, see `vendor/LICENSE`).
- nginx itself remains BSD-licensed; this module links dynamically.
