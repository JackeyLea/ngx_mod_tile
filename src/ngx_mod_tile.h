/*
 * ngx_mod_tile - nginx port of OpenStreetMap's mod_tile
 *
 * Shared declarations for the module sources. Serves map tiles from
 * mod_tile-compatible metatile stores, submitting on-demand render
 * requests to renderd over its v2/v3 wire protocol using a fully
 * asynchronous connection state machine.
 */

#ifndef NGX_MOD_TILE_H
#define NGX_MOD_TILE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "protocol.h"
#include "store.h"
#include "tile_stats_shm.h"
#include "tile_delaypool_shm.h"

/*
 * Maximum zoom tracked by server-side configuration (same as
 * mod_tile's MAX_ZOOM_SERVER). renderd.conf imports are additionally
 * clamped to MAX_ZOOM (20) by the upstream validation rules.
 */
#define NGX_TILE_MAX_ZOOM_SERVER 30

/* nginx 1.30 has no NGX_CONF_UNSET_DOUBLE sentinel */
#define NGX_TILE_UNSET_DOUBLE ((double) NGX_CONF_UNSET)

/*
 * Cache/render outcome codes, mirroring mod_tile.h. Used for logging
 * now; the statistics counters hook in here in stage 2.
 */
#define NGX_TILE_FRESH           1
#define NGX_TILE_OLD             2
#define NGX_TILE_FRESH_RENDER    3
#define NGX_TILE_OLD_RENDER      4
#define NGX_TILE_VERYOLD_RENDER  5
#define NGX_TILE_VERYOLD         6

/* Default openstreetmap attribution string (mod_tile.h DEFAULT_ATTRIBUTION) */
#define NGX_TILE_DEFAULT_ATTRIBUTION \
    "&copy;<a href='http://www.openstreetmap.org/'>OpenStreetMap</a> and " \
    "<a href='http://wiki.openstreetmap.org/wiki/Contributors'>contributors</a>, " \
    "<a href='http://opendatacommons.org/licenses/odbl/'>(ODbL)</a>"

/* Defaults carried over from mod_tile / render_config.h (in msec) */
#define NGX_TILE_REQUEST_TIMEOUT          3000
#define NGX_TILE_MISSING_REQUEST_TIMEOUT  3000
#define NGX_TILE_CONNECT_TIMEOUT          1000
#define NGX_TILE_VERY_OLD_THRESHOLD_MS    (365L * 24 * 60 * 60 * 1000)
#define NGX_TILE_CACHE_DURATION_DIRTY     900
#define NGX_TILE_CACHE_DURATION_MAX       604800
#define NGX_TILE_CACHE_DURATION_MINIMUM   10800
#define NGX_TILE_CACHE_DURATION_LOW       518400
#define NGX_TILE_CACHE_DURATION_MEDIUM    86400
#define NGX_TILE_MAX_LOAD_OLD             16
#define NGX_TILE_MAX_LOAD_MISSING         50

/** @brief Endpoint requested within a layer URI. */
#define NGX_TILE_EP_SERVE  0
#define NGX_TILE_EP_STATUS 1
#define NGX_TILE_EP_DIRTY  2
#define NGX_TILE_EP_JSON   3

/** @brief Freshness classification of a tile in the storage backend. */
typedef enum {
    ngx_tile_unknown = 0,
    ngx_tile_missing,
    ngx_tile_old,
    ngx_tile_very_old,
    ngx_tile_current
} ngx_tile_state_e;

/**
 * @brief A resolved tile layer: either configured via a location's
 * directives (tile_layer ...) or imported from renderd.conf via
 * tile_import_config.
 */
typedef struct {
    ngx_str_t     xmlname;          /* map/style name sent to renderd */
    ngx_str_t     baseuri;          /* URI prefix under which tiles live */
    ngx_str_t     file_extension;   /* e.g. "png" */
    ngx_str_t     mime_type;        /* e.g. "image/png" */
    ngx_int_t     minzoom;
    ngx_int_t     maxzoom;
    ngx_int_t     aspect_x;
    ngx_int_t     aspect_y;
    ngx_str_t     attribution;      /* TileJSON attribution (empty = default) */
    ngx_str_t     description;      /* TileJSON description */
    ngx_str_t     cors;             /* CORS policy; empty = disabled */
    ngx_str_t     store_uri;        /* tile dir or backend URI (NUL-terminated) */
    ngx_array_t  *hostnames;        /* ngx_str_t TileJSON URL prefixes; may be NULL */
    ngx_flag_t    enable_options;   /* /<options>/z/x/y.ext/<option> URLs enabled */
    ngx_uint_t    stats_index;      /* per-layer counter index in the stats zone */
} ngx_http_tile_layer_t;

/**
 * @brief Per-request state, also threaded through the asynchronous
 * renderd connection (mirrors mod_tile's tile_request_data + more).
 */
typedef struct {
    ngx_http_request_t     *r;
    ngx_http_tile_layer_t  *layer;
    ngx_pool_cleanup_t     *cln;       /* cleanup registered on r->pool */
    struct protocol         cmd;       /* wire packet to send (v2 or v3) */
    struct protocol         resp;      /* reply packet */
    size_t                  nrecv;     /* reply bytes received so far */
    size_t                  want;      /* expected reply size (61 or 143) */
    ngx_int_t               priority;  /* 0 dirty, 1 low, 2 normal, 3 prio */
    ngx_tile_state_e        tile_state;
    struct storage_backend *store;
    ngx_uint_t              endpoint;  /* NGX_TILE_EP_* */
    ngx_uint_t              resp_status;   /* final status, for the counters */
    ngx_uint_t              thread_stage;  /* NGX_TILE_THREAD_* offload state */
    unsigned                stat_done:1;   /* stat_result populated */
    void                   *thread_ctx;    /* ngx_http_tile_thread_t while pending */
    ngx_pool_cleanup_t     *thread_cleanup;
    struct stat_info        st_result; /* stat result from the thread task */
    unsigned                oob:1;     /* URI parsed but coordinates out of bounds */
    time_t                  mtime;     /* tile mtime from tile_stat (sec) */
    time_t                  atime;
    ngx_int_t               fresh_code;/* NGX_TILE_* outcome for logging/stats */
    /* renderd connection (NULL until a render/dirty submit is started) */
    void                   *conn;      /* ngx_http_tile_conn_t */
    unsigned                head_only:1;
} ngx_http_tile_ctx_t;

/**
 * @brief Per-location module configuration. Location-scope directives
 * populate the embedded layer; server-scope settings are inherited via
 * merge_loc_conf. renderd.conf imports live in the server-level
 * (default) instance of this struct.
 */
typedef struct {
    ngx_http_tile_layer_t  layer;      /* active when layer.xmlname.len > 0 */

    /* renderd endpoint: "unix:/path" or "host:port" */
    ngx_str_t               renderd_socket;
    ngx_str_t               renderd_host;   /* for TCP mode */
    ngx_int_t               renderd_port;   /* 0 = unix socket mode */
    ngx_str_t               socket_path;    /* for unix mode (NUL-terminated) */

    ngx_str_t               tile_dir;
    ngx_msec_t              request_timeout;
    ngx_msec_t              request_timeout_priority;
    ngx_msec_t              connect_timeout;
    ngx_int_t               max_load_old;
    ngx_int_t               max_load_missing;
    ngx_msec_t              very_old_threshold;

    double                  cache_duration_last_modified_factor;
    ngx_int_t               cache_duration_dirty;
    ngx_int_t               cache_duration_max;
    ngx_int_t               cache_duration_minimum;
    ngx_int_t               cache_duration_low_zoom;
    ngx_int_t               cache_duration_medium_zoom;
    ngx_int_t               cache_level_low_zoom;
    ngx_int_t               cache_level_medium_zoom;
    ngx_int_t               cache_extended_duration;
    ngx_str_t               cache_extended_hostname;
    ngx_int_t               mincachetime[NGX_TILE_MAX_ZOOM_SERVER + 1];

    ngx_flag_t              enable_bulk_mode;
    ngx_flag_t              enable_dirty_url;
    ngx_flag_t              enable_status_url;

    /* server-scope: layers imported from renderd.conf */
    ngx_str_t               import_config;
    ngx_array_t            *imported_layers; /* ngx_http_tile_layer_t */

    /* stage 2: statistics + per-IP throttling */
    ngx_flag_t              enable_stats;     /* tile_enable_stats, default on */
    ngx_flag_t              stats_page;       /* tile_stats on -> /mod_tile page */
    ngx_flag_t              metrics_page;     /* tile_metrics on -> /metrics page */
    ngx_int_t               throttle_tile_bucket;   /* default 5000 */
    ngx_int_t               throttle_render_bucket; /* default 65 */
    ngx_msec_t              throttle_tile_interval;   /* ms per tile token, default 1000 */
    ngx_msec_t              throttle_render_interval; /* ms per render token, default 5000 */
    ngx_int_t               throttle_xforward;        /* 0 off, 1 first XFF, 2 last XFF */
    ngx_flag_t              throttle_enabled;         /* tile_throttling on, default off */
    ngx_array_t            *throttle_whitelist;       /* in_addr_t, up to 13 */
    ngx_thread_pool_t      *thread_pool;              /* tile_thread_pool, NULL = sync */
} ngx_http_tile_loc_conf_t;

/** @brief Shared storage-backend instance cached per worker. */
typedef struct {
    ngx_str_t                uri;     /* NUL-terminated backend options string */
    struct storage_backend  *store;
} ngx_http_tile_store_t;

/** @brief Module main configuration: shared caches and registries. */
typedef struct {
    ngx_array_t   *stores;       /* ngx_http_tile_store_t */

    /* stage 2: statistics */
    ngx_str_t      stats_zone_size_str;    /* tile_stats_zone (http scope), default "4m" */
    ngx_str_t      throttle_zone_size_str; /* tile_throttling_zone (http scope), empty = off */
    ngx_uint_t     layer_count;      /* assigned at config parse, size of per-layer arrays */
    ngx_uint_t     global_max_zoom;  /* max zoom over all layers, for stats output */
    ngx_array_t   *layers_registry;  /* ngx_http_tile_layer_t* in parse order */
    ngx_shm_zone_t *stats_zone;
    ngx_http_tile_stats_shm_t *stats;      /* shm counters, NULL when zone failed */
    ngx_slab_pool_t *stats_slab;

    /* stage 2: per-IP delaypool */
    ngx_shm_zone_t *throttle_zone;
    ngx_tile_delaypool_shm_t *delaypool;   /* NULL when throttling disabled */
    ngx_slab_pool_t *delaypool_slab;
} ngx_http_tile_main_conf_t;

extern ngx_module_t ngx_http_tile_module;

/* tile_config.c */
void ngx_http_tile_apply_defaults(ngx_conf_t *cf,
    ngx_http_tile_loc_conf_t *tlcf);
char *ngx_http_tile_import_config(ngx_conf_t *cf,
    ngx_http_tile_loc_conf_t *tlcf);

/* tile_store.c */
struct storage_backend *ngx_http_tile_get_store(
    ngx_http_tile_main_conf_t *mcf, ngx_str_t *store_uri, ngx_log_t *log);
void ngx_http_tile_close_stores(ngx_http_tile_main_conf_t *mcf);

/* tile_serve.c */
ngx_int_t ngx_http_tile_matcher(ngx_http_request_t *r);
ngx_int_t ngx_http_tile_content(ngx_http_request_t *r);
ngx_int_t ngx_http_tile_check_state(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, ngx_int_t render_rc);

/* tile_renderd.c */
ngx_int_t ngx_http_tile_render_submit(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, ngx_http_tile_loc_conf_t *tlcf,
    ngx_int_t priority);
void ngx_http_tile_dirty_submit_detached(ngx_http_request_t *r,
    ngx_http_tile_loc_conf_t *tlcf, struct protocol *cmd);

/* tile_response.c */
ngx_int_t ngx_http_tile_send_response(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
ngx_int_t ngx_http_tile_send_text(ngx_http_request_t *r, ngx_uint_t status,
    const char *body);
ngx_int_t ngx_http_tile_output_tilejson(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
ngx_int_t ngx_http_tile_cors(ngx_http_request_t *r, ngx_str_t *cors);
void ngx_http_tile_add_expiry(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);

/* tile_endpoints.c */
ngx_int_t ngx_http_tile_endpoint_status(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
ngx_int_t ngx_http_tile_endpoint_dirty(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);

/* tile_stats.c / tile_delaypool.c (stage 2) */
ngx_int_t ngx_http_tile_stats_init_zone_conf(ngx_conf_t *cf,
    ngx_http_tile_main_conf_t *mcf);
ngx_int_t ngx_http_tile_delaypool_init_zone_conf(ngx_conf_t *cf,
    ngx_http_tile_main_conf_t *mcf);
void ngx_http_tile_stats_response(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, ngx_uint_t status);
void ngx_http_tile_stats_fresh(ngx_http_tile_main_conf_t *mcf, ngx_int_t code);
void ngx_http_tile_stats_timing(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, ngx_int_t usec);
ngx_int_t ngx_http_tile_stats_page(ngx_http_request_t *r);
ngx_int_t ngx_http_tile_metrics_page(ngx_http_request_t *r);
ngx_int_t ngx_http_tile_delay_allowed(ngx_http_request_t *r,
    ngx_http_tile_loc_conf_t *tlcf, ngx_tile_state_e state);

/* tile_thread.c (stage 3) */
ngx_int_t ngx_http_tile_serve_flow_after_stat(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
ngx_int_t ngx_http_tile_send_response_after_read(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, u_char *buf, ssize_t len, int compressed,
    const char *err_msg);
ngx_int_t ngx_http_tile_thread_stat(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
ngx_int_t ngx_http_tile_thread_read(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
ngx_int_t ngx_http_tile_thread_resume(ngx_http_request_t *r, ngx_uint_t stage);

/* tile_store.c: per-thread store cache for the thread pool path */
struct storage_backend *ngx_http_tile_get_store_thread(
    ngx_http_tile_main_conf_t *mcf, ngx_str_t *store_uri, ngx_log_t *log);
void ngx_http_tile_store_tls_destroy(void);

#endif /* NGX_MOD_TILE_H */
