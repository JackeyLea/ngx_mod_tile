/*
 * ngx_mod_tile - shared statistics structures (stage 2).
 *
 * Layout mirrors mod_tile's stats_data (mod_tile.h). All counters are
 * ngx_atomic_uint_t so workers increment without a lock; readers copy the
 * block under the slab mutex for a consistent snapshot. The per-layer
 * arrays are appended to the block in the zone, sized mcf->layer_count.
 */

#ifndef NGX_TILE_STATS_SHM_H
#define NGX_TILE_STATS_SHM_H

#include <ngx_config.h>
#include <ngx_core.h>

#define NGX_TILE_STATS_ZONE_NAME "ngx_mod_tile_stats"
#define NGX_TILE_STATS_ZONE_DEFAULT "4m"

/* defined in ngx_mod_tile.h; the shm headers may be read first */
#ifndef NGX_TILE_MAX_ZOOM_SERVER
#define NGX_TILE_MAX_ZOOM_SERVER 30
#endif

typedef struct {
    ngx_atomic_uint_t noResp200;
    ngx_atomic_uint_t noResp304;
    ngx_atomic_uint_t noResp404;
    ngx_atomic_uint_t noResp503;
    ngx_atomic_uint_t noResp5XX;
    ngx_atomic_uint_t noRespOther;

    ngx_atomic_uint_t noFreshCache;
    ngx_atomic_uint_t noFreshRender;
    ngx_atomic_uint_t noOldCache;
    ngx_atomic_uint_t noOldRender;
    ngx_atomic_uint_t noVeryOldCache;
    ngx_atomic_uint_t noVeryOldRender;

    ngx_atomic_uint_t noRespZoom[NGX_TILE_MAX_ZOOM_SERVER + 1];

    ngx_atomic_uint_t totalBufferRetrievalTime;   /* usec */
    ngx_atomic_uint_t noTotalBufferRetrieval;
    ngx_atomic_uint_t zoomBufferRetrievalTime[NGX_TILE_MAX_ZOOM_SERVER + 1];
    ngx_atomic_uint_t noZoomBufferRetrieval[NGX_TILE_MAX_ZOOM_SERVER + 1];

    /* per-layer counters, sized mcf->layer_count, allocated after this struct */
    ngx_atomic_uint_t *noResp200Layer;
    ngx_atomic_uint_t *noResp404Layer;
} ngx_http_tile_stats_shm_t;

#endif /* NGX_TILE_STATS_SHM_H */
