/*
 * ngx_mod_tile - shared statistics (stage 2).
 *
 * Port of mod_tile's incRespCounter / incFreshCounter / incTimingCounter and
 * the /mod_tile (tile_handler_mod_stats) and /metrics (tile_handler_metrics)
 * endpoints, backed by an nginx shared-memory zone with atomic counters.
 * Counters are global across servers (upstream enable_global_stats = 1);
 * per-server tile_enable_stats only gates whether requests count.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <stdio.h>
#include <sys/time.h>

#include "ngx_mod_tile.h"
#include "tile_stats.h"

static ngx_int_t ngx_http_tile_stats_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static u_char *ngx_http_tile_stats_copy(ngx_http_request_t *r,
    ngx_http_tile_stats_shm_t *copy);

/* outcome codes -> counter, port of incRespCounter */
void
ngx_http_tile_stats_response(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, ngx_uint_t status)
{
    ngx_http_tile_loc_conf_t  *tlcf;
    ngx_http_tile_main_conf_t *mcf;
    ngx_atomic_uint_t         *counter = NULL;
    ngx_uint_t                  layer;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);
    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    if (!tlcf->enable_stats || mcf->stats == NULL) {
        return;
    }

    switch (status) {
    case NGX_HTTP_OK:
        counter = &mcf->stats->noResp200;
        break;
    case NGX_HTTP_NOT_MODIFIED:
        counter = &mcf->stats->noResp304;
        break;
    case NGX_HTTP_NOT_FOUND:
        counter = &mcf->stats->noResp404;
        break;
    case NGX_HTTP_SERVICE_UNAVAILABLE:
        counter = &mcf->stats->noResp503;
        break;
    case NGX_HTTP_INTERNAL_SERVER_ERROR:
        counter = &mcf->stats->noResp5XX;
        break;
    default:
        counter = &mcf->stats->noRespOther;
        break;
    }

    (void) ngx_atomic_fetch_add(counter, 1);

    if (ctx == NULL) {
        return;
    }

    /* zoom counters follow the 200/304 group only, like upstream */
    if (status == NGX_HTTP_OK || status == NGX_HTTP_NOT_MODIFIED) {
        if (ctx->cmd.z >= 0 && ctx->cmd.z <= NGX_TILE_MAX_ZOOM_SERVER) {
            (void) ngx_atomic_fetch_add(
                &mcf->stats->noRespZoom[ctx->cmd.z], 1);
        }

        layer = ctx->layer->stats_index;

        if (mcf->layer_count > 0 && layer < mcf->layer_count) {
            /* upstream counts 304s into the per-layer 200 counter too */
            (void) ngx_atomic_fetch_add(&mcf->stats->noResp200Layer[layer], 1);
        }
    } else if (status == NGX_HTTP_NOT_FOUND) {
        layer = ctx->layer->stats_index;

        if (mcf->layer_count > 0 && layer < mcf->layer_count) {
            (void) ngx_atomic_fetch_add(&mcf->stats->noResp404Layer[layer], 1);
        }
    }
}

/* port of incFreshCounter */
void
ngx_http_tile_stats_fresh(ngx_http_tile_main_conf_t *mcf, ngx_int_t code)
{
    ngx_atomic_uint_t *counter = NULL;

    if (mcf->stats == NULL) {
        return;
    }

    switch (code) {
    case NGX_TILE_FRESH:
        counter = &mcf->stats->noFreshCache;
        break;
    case NGX_TILE_FRESH_RENDER:
        counter = &mcf->stats->noFreshRender;
        break;
    case NGX_TILE_OLD:
        counter = &mcf->stats->noOldCache;
        break;
    case NGX_TILE_OLD_RENDER:
        counter = &mcf->stats->noOldRender;
        break;
    case NGX_TILE_VERYOLD:
        counter = &mcf->stats->noVeryOldCache;
        break;
    case NGX_TILE_VERYOLD_RENDER:
        counter = &mcf->stats->noVeryOldRender;
        break;
    default:
        return;
    }

    (void) ngx_atomic_fetch_add(counter, 1);
}

/* port of incTimingCounter: duration in microseconds around tile_read */
void
ngx_http_tile_stats_timing(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, ngx_int_t usec)
{
    ngx_http_tile_loc_conf_t  *tlcf;
    ngx_http_tile_main_conf_t *mcf;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);
    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    if (!tlcf->enable_stats || mcf->stats == NULL) {
        return;
    }

    if (ctx->cmd.z < 0 || ctx->cmd.z > NGX_TILE_MAX_ZOOM_SERVER) {
        return;
    }

    (void) ngx_atomic_fetch_add(&mcf->stats->totalBufferRetrievalTime,
                                (ngx_atomic_uint_t) usec);
    (void) ngx_atomic_fetch_add(&mcf->stats->noTotalBufferRetrieval, 1);
    (void) ngx_atomic_fetch_add(
        &mcf->stats->zoomBufferRetrievalTime[ctx->cmd.z],
        (ngx_atomic_uint_t) usec);
    (void) ngx_atomic_fetch_add(&mcf->stats->noZoomBufferRetrieval[ctx->cmd.z],
                                1);
}

/* ------------------------------------------------------------------ */
/* zone lifecycle                                                      */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_stats_init_zone_conf(ngx_conf_t *cf,
    ngx_http_tile_main_conf_t *mcf)
{
    ngx_str_t  name = ngx_string(NGX_TILE_STATS_ZONE_NAME);
    size_t     size;
    ngx_str_t  sz;

    sz = mcf->stats_zone_size_str;

    size = (size_t) ngx_parse_size(&sz);
    if (size == (size_t) NGX_ERROR || size < sizeof(ngx_http_tile_stats_shm_t) + 64) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "tile_stats_zone: invalid size \"%V\"", &sz);
        return NGX_ERROR;
    }

    mcf->stats_zone = ngx_shared_memory_add(cf, &name, size,
                                            &ngx_http_tile_module);
    if (mcf->stats_zone == NULL) {
        return NGX_ERROR;
    }

    mcf->stats_zone->init = ngx_http_tile_stats_init_zone;
    mcf->stats_zone->data = mcf;

    return NGX_OK;
}

static ngx_int_t
ngx_http_tile_stats_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_tile_main_conf_t  *omcf = data;
    ngx_http_tile_main_conf_t  *mcf = shm_zone->data;
    ngx_slab_pool_t            *shpool;
    size_t                       arrays_size;
    u_char                      *p;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    /* keep counters across reloads when the layer set did not change */
    if (omcf != NULL && omcf->stats != NULL
        && omcf->layer_count == mcf->layer_count)
    {
        mcf->stats = omcf->stats;
        mcf->stats_slab = omcf->stats_slab;
        return NGX_OK;
    }

    arrays_size = sizeof(ngx_http_tile_stats_shm_t)
                  + mcf->layer_count * 2 * sizeof(ngx_atomic_uint_t);

    ngx_shmtx_lock(&shpool->mutex);

    p = ngx_slab_alloc_locked(shpool, arrays_size);

    ngx_shmtx_unlock(&shpool->mutex);

    if (p == NULL) {
        ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                      "tile: failed to allocate %uz bytes in the stats zone",
                      arrays_size);
        return NGX_ERROR;
    }

    ngx_memzero(p, arrays_size);

    mcf->stats = (ngx_http_tile_stats_shm_t *) p;
    mcf->stats->noResp200Layer = (ngx_atomic_uint_t *)
        (p + sizeof(ngx_http_tile_stats_shm_t));
    mcf->stats->noResp404Layer = mcf->stats->noResp200Layer + mcf->layer_count;
    mcf->stats_slab = shpool;

    return NGX_OK;
}

/* consistent snapshot for the page handlers */
static u_char *
ngx_http_tile_stats_copy(ngx_http_request_t *r,
    ngx_http_tile_stats_shm_t *copy)
{
    ngx_http_tile_main_conf_t *mcf;
    ngx_slab_pool_t           *shpool;
    size_t                      arrays_size;

    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);
    shpool = mcf->stats_slab;

    arrays_size = sizeof(ngx_http_tile_stats_shm_t)
                  + mcf->layer_count * 2 * sizeof(ngx_atomic_uint_t);

    if (shpool != NULL) {
        ngx_shmtx_lock(&shpool->mutex);
        ngx_memcpy(copy, mcf->stats, arrays_size);
        ngx_shmtx_unlock(&shpool->mutex);
    }

    return (u_char *) copy;
}

/* ------------------------------------------------------------------ */
/* /mod_tile page (port of tile_handler_mod_stats)                     */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_stats_page(ngx_http_request_t *r)
{
    ngx_http_tile_loc_conf_t  *tlcf;
    ngx_http_tile_main_conf_t *mcf;
    ngx_http_tile_stats_shm_t  stats;
    ngx_http_tile_layer_t     *layer;
    size_t                      size;
    u_char                     *buf, *p;
    ngx_uint_t                  i;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);

    if (!tlcf->enable_stats) {
        return ngx_http_tile_send_text(r, NGX_HTTP_OK,
            "Stats are not enabled for this server");
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    if (mcf->stats == NULL) {
        return ngx_http_tile_send_text(r, NGX_HTTP_OK,
            "Stats are not enabled for this server");
    }

    ngx_http_tile_stats_copy(r, &stats);

    size = 1024 + 96 * (NGX_TILE_MAX_ZOOM_SERVER + 1)
           + 128 * (mcf->layer_count + 1);

    buf = ngx_pcalloc(r->pool, size);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = buf;

    p = ngx_snprintf(p, buf + size - p,
        "NoResp200: %uL\n"
        "NoResp304: %uL\n"
        "NoResp404: %uL\n"
        "NoResp503: %uL\n"
        "NoResp5XX: %uL\n"
        "NoRespOther: %uL\n"
        "NoFreshCache: %uL\n"
        "NoOldCache: %uL\n"
        "NoVeryOldCache: %uL\n"
        "NoFreshRender: %uL\n"
        "NoOldRender: %uL\n"
        "NoVeryOldRender: %uL\n",
        stats.noResp200, stats.noResp304, stats.noResp404, stats.noResp503,
        stats.noResp5XX, stats.noRespOther, stats.noFreshCache,
        stats.noOldCache, stats.noVeryOldCache, stats.noFreshRender,
        stats.noOldRender, stats.noVeryOldRender);

    for (i = 0; i <= mcf->global_max_zoom; i++) {
        p = ngx_snprintf(p, buf + size - p, "NoRespZoom%02ui: %uL\n",
                         i, stats.noRespZoom[i]);
    }

    p = ngx_snprintf(p, buf + size - p,
        "NoTileBufferReads: %uL\n"
        "DurationTileBufferReads: %uL\n",
        stats.noTotalBufferRetrieval, stats.totalBufferRetrievalTime);

    for (i = 0; i <= mcf->global_max_zoom; i++) {
        p = ngx_snprintf(p, buf + size - p,
            "NoTileBufferReadZoom%02ui: %uL\n"
            "DurationTileBufferReadZoom%02ui: %uL\n",
            i, stats.noZoomBufferRetrieval[i],
            i, stats.zoomBufferRetrievalTime[i]);
    }

    for (i = 0; i < mcf->layer_count && i < mcf->layers_registry->nelts; i++) {
        layer = ((ngx_http_tile_layer_t **) mcf->layers_registry->elts)[i];

        p = ngx_snprintf(p, buf + size - p,
            "NoRes200Layer%V: %uL\n"
            "NoRes404Layer%V: %uL\n",
            &layer->baseuri, stats.noResp200Layer[i],
            &layer->baseuri, stats.noResp404Layer[i]);
    }

    return ngx_http_tile_send_text(r, NGX_HTTP_OK, (const char *) buf);
}

/* ------------------------------------------------------------------ */
/* /metrics page (port of tile_handler_metrics)                        */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_metrics_page(ngx_http_request_t *r)
{
    ngx_http_tile_loc_conf_t  *tlcf;
    ngx_http_tile_main_conf_t *mcf;
    ngx_http_tile_stats_shm_t  stats;
    ngx_http_tile_layer_t     *layer;
    size_t                      size;
    u_char                     *buf, *p;
    ngx_uint_t                  i;
    char                        secs[64];

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);

    if (!tlcf->enable_stats) {
        return ngx_http_tile_send_text(r, NGX_HTTP_OK,
            "Stats are not enabled for this server");
    }

    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    if (mcf->stats == NULL) {
        return ngx_http_tile_send_text(r, NGX_HTTP_OK,
            "Stats are not enabled for this server");
    }

    ngx_http_tile_stats_copy(r, &stats);

    size = 4096 + 160 * (NGX_TILE_MAX_ZOOM_SERVER + 1)
           + 256 * (mcf->layer_count + 1);

    buf = ngx_pcalloc(r->pool, size);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = buf;

    p = ngx_snprintf(p, buf + size - p,
        "# HELP modtile_http_responses_total Number of HTTP responses by response code\n"
        "# TYPE modtile_http_responses_total counter\n"
        "modtile_http_responses_total{status=\"200\"} %uL\n"
        "modtile_http_responses_total{status=\"304\"} %uL\n"
        "modtile_http_responses_total{status=\"404\"} %uL\n"
        "modtile_http_responses_total{status=\"503\"} %uL\n"
        "modtile_http_responses_total{status=\"5XX\"} %uL\n"
        "modtile_http_responses_total{status=\"other\"} %uL\n",
        stats.noResp200, stats.noResp304, stats.noResp404, stats.noResp503,
        stats.noResp5XX, stats.noRespOther);

    p = ngx_snprintf(p, buf + size - p,
        "# HELP modtile_tiles_total Number of tiles served by state\n"
        "# TYPE modtile_tiles_total counter\n"
        "modtile_tiles_total{age=\"fresh\",rendered=\"no\"} %uL\n"
        "modtile_tiles_total{age=\"old\",rendered=\"no\"} %uL\n"
        "modtile_tiles_total{age=\"outdated\",rendered=\"no\"} %uL\n"
        "modtile_tiles_total{age=\"fresh\",rendered=\"yes\"} %uL\n"
        "modtile_tiles_total{age=\"old\",rendered=\"attempted\"} %uL\n"
        "modtile_tiles_total{age=\"outdated\",rendered=\"attempted\"} %uL\n",
        stats.noFreshCache, stats.noOldCache, stats.noVeryOldCache,
        stats.noFreshRender, stats.noOldRender, stats.noVeryOldRender);

    p = ngx_snprintf(p, buf + size - p,
        "# HELP modtile_zoom_responses_total Number of HTTP responses by zoom\n"
        "# TYPE modtile_zoom_responses_total counter\n");
    for (i = 0; i <= mcf->global_max_zoom; i++) {
        p = ngx_snprintf(p, buf + size - p,
            "modtile_zoom_responses_total{zoom=\"%02ui\"} %uL\n",
            i, stats.noRespZoom[i]);
    }

    p = ngx_snprintf(p, buf + size - p,
        "# HELP modtile_tile_reads_total Number of tile reads by zoom\n"
        "# TYPE modtile_tile_reads_total counter\n");
    for (i = 0; i <= mcf->global_max_zoom; i++) {
        p = ngx_snprintf(p, buf + size - p,
            "modtile_tile_reads_total{zoom=\"%02ui\"} %uL\n",
            i, stats.noZoomBufferRetrieval[i]);
    }

    p = ngx_snprintf(p, buf + size - p,
        "# HELP modtile_tile_reads_seconds_total Total time reading tiles by zoom\n"
        "# TYPE modtile_tile_reads_seconds_total counter\n");
    for (i = 0; i <= mcf->global_max_zoom; i++) {
        (void) snprintf(secs, sizeof(secs), "%lf",
                        stats.zoomBufferRetrievalTime[i] / 1000000.0);
        p = ngx_snprintf(p, buf + size - p,
            "modtile_tile_reads_seconds_total{zoom=\"%02ui\"} %s\n",
            i, secs);
    }

    p = ngx_snprintf(p, buf + size - p,
        "# HELP modtile_layer_responses_total Number of responses by layer\n"
        "# TYPE modtile_layer_responses_total counter\n");
    for (i = 0; i < mcf->layer_count && i < mcf->layers_registry->nelts; i++) {
        layer = ((ngx_http_tile_layer_t **) mcf->layers_registry->elts)[i];

        p = ngx_snprintf(p, buf + size - p,
            "modtile_layer_responses_total{layer=\"%V\",status=\"200\"} %uL\n"
            "modtile_layer_responses_total{layer=\"%V\",status=\"404\"} %uL\n",
            &layer->baseuri, stats.noResp200Layer[i],
            &layer->baseuri, stats.noResp404Layer[i]);
    }

    return ngx_http_tile_send_text(r, NGX_HTTP_OK, (const char *) buf);
}
