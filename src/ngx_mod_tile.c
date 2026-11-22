/*
 * ngx_mod_tile - module entry point.
 *
 * nginx port of OpenStreetMap's Apache mod_tile. Registers the
 * preaccess-phase matcher that claims tile requests via
 * r->content_handler, plus the module lifecycle hooks (per-worker
 * storage backend cache teardown).
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"
#include "tile_config.h"

static void *ngx_http_tile_create_main_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_tile_init(ngx_conf_t *cf);
static void ngx_http_tile_exit_process(ngx_cycle_t *cycle);

static ngx_http_module_t ngx_http_tile_module_ctx = {
    NULL,                             /* preconfiguration */
    ngx_http_tile_init,               /* postconfiguration */

    ngx_http_tile_create_main_conf,   /* create main configuration */
    NULL,                             /* init main configuration */

    NULL,                             /* create server configuration */
    NULL,                             /* merge server configuration */

    ngx_http_tile_create_loc_conf,    /* create location configuration */
    ngx_http_tile_merge_loc_conf      /* merge location configuration */
};

ngx_module_t ngx_http_tile_module = {
    NGX_MODULE_V1,
    &ngx_http_tile_module_ctx,        /* module context */
    ngx_http_tile_commands,           /* module directives */
    NGX_HTTP_MODULE,                  /* module type */
    NULL,                             /* init master */
    NULL,                             /* init module */
    NULL,                             /* init process */
    NULL,                             /* init thread */
    NULL,                             /* exit thread */
    ngx_http_tile_exit_process,       /* exit process */
    NULL,                             /* exit master */
    NGX_MODULE_V1_PADDING
};

static void *
ngx_http_tile_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_tile_main_conf_t *mcf;

    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_tile_main_conf_t));
    if (mcf == NULL) {
        return NULL;
    }

    mcf->stores = ngx_array_create(cf->pool, 8, sizeof(ngx_http_tile_store_t));
    if (mcf->stores == NULL) {
        return NULL;
    }

    return mcf;
}

static ngx_int_t
ngx_http_tile_init(ngx_conf_t *cf)
{
    ngx_http_core_main_conf_t *cmcf;
    ngx_http_handler_pt       *h;

    /* claim matching requests before the access checks and the static
     * content handler (r->content_handler is called by the core content
     * phase, so allow/deny directives in tile locations still apply) */
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_tile_matcher;

    return NGX_OK;
}

static void
ngx_http_tile_exit_process(ngx_cycle_t *cycle)
{
    ngx_http_tile_main_conf_t *mcf;

    mcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_tile_module);
    if (mcf == NULL) {
        return;
    }

    ngx_http_tile_close_stores(mcf);
}
