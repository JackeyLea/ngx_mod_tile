/*
 * ngx_mod_tile - request routing and the tile-serving flow.
 *
 * ngx_http_tile_matcher() runs in the preaccess phase: it matches the
 * request URI against the active layer (location-scoped tile_layer or a
 * server-scoped tile_import_config entry), parses and validates the tile
 * path exactly like mod_tile's tile_translate(), and claims the request
 * by setting r->content_handler so the core content phase calls
 * ngx_http_tile_content() regardless of handler registration order.
 *
 * ngx_http_tile_content() implements the tile_storage_hook +
 * tile_handler_serve logic: freshness classification, load-based load
 * shedding, on-demand rendering through the asynchronous renderd engine
 * (suspending the request), and the final response.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"
#include "tile_serve.h"
#include "tile_renderd.h"
#include "sys_utils.h"
#include <ngx_md5.h>
#include <string.h>

extern ngx_module_t ngx_http_core_module;

static ngx_http_tile_ctx_t *ngx_http_tile_parse_uri(ngx_http_request_t *r,
    ngx_http_tile_layer_t *layer, const u_char *rest, size_t rest_len);
static ngx_int_t ngx_http_tile_serve_flow(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
static ngx_tile_state_e ngx_http_tile_classify(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx, struct stat_info st);
static ngx_int_t ngx_http_tile_parse_int(const u_char *data, size_t len,
    int *dest);

/* ------------------------------------------------------------------ */
/* URI parsing (port of tile_translate)                                */
/* ------------------------------------------------------------------ */

static ngx_int_t
ngx_http_tile_parse_int(const u_char *data, size_t len, int *dest)
{
    u_char  c;
    u_long  val = 0;
    size_t  i;

    if (len == 0) {
        return NGX_ERROR;
    }

    for (i = 0; i < len; i++) {
        c = data[i];
        if (c < '0' || c > '9') {
            return NGX_ERROR;
        }
        val = val * 10 + (c - '0');
        if (val > 2147483647 / 10) {
            return NGX_ERROR;
        }
    }

    *dest = (int) val;
    return NGX_OK;
}

/*
 * Parse "<z>/<x>/<y>.<ext>[/status|/dirty]" or
 * "<parameters>/<z>/<x>/<y>.<ext>[/<option>]" after the baseuri.
 *
 * Mirrors mod_tile's tile_translate token splitting and validation:
 *  - unparsable coordinates or wrong extension -> NULL (DECLINED upstream)
 *  - coordinates out of bounds -> ctx->oob set (404 upstream)
 */
static ngx_http_tile_ctx_t *
ngx_http_tile_parse_uri(ngx_http_request_t *r, ngx_http_tile_layer_t *layer,
    const u_char *rest, size_t rest_len)
{
    ngx_http_tile_ctx_t  *ctx;
    u_char               *path, *parts[5], *tok, *y_str;
    char                 *sp;
    ngx_uint_t            n_parts = 0, i;
    size_t                y_len, ext_len;
    u_char               *dot;
    int                   z = 0, x = 0, y = 0;
    ngx_uint_t            limit;
    unsigned              oob;

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_tile_ctx_t));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->r = r;
    ctx->layer = layer;
    ctx->endpoint = NGX_TILE_EP_SERVE;

    if (rest_len == sizeof("tile-layer.json") - 1
        && ngx_memcmp(rest, "tile-layer.json", rest_len) == 0)
    {
        ctx->endpoint = NGX_TILE_EP_JSON;
        return ctx;
    }

    /* split on '/': keep the first 5 tokens; more tokens collapse into a
     * sentinel count of 6, mirroring the upstream else-branches */
    path = ngx_pnalloc(r->pool, rest_len + 1);
    if (path == NULL) {
        return NULL;
    }
    ngx_memcpy(path, rest, rest_len);
    path[rest_len] = '\0';

    tok = (u_char *) strtok_r((char *) path, "/", &sp);
    while (tok != NULL && n_parts < 5) {
        parts[n_parts++] = tok;
        tok = (u_char *) strtok_r(NULL, "/", &sp);
    }

    if (tok != NULL) {
        n_parts = 6;
    }

    if (n_parts == 0) {
        return NULL;
    }

    ext_len = layer->file_extension.len;

    if (layer->enable_options) {
        /* /parameters/z/x/y.ext[/option] */
        size_t  params_len;

        if (n_parts < 4) {
            return NULL;
        }

        ctx->cmd.ver = PROTO_VER;

        params_len = ngx_strlen(parts[0]);
        if (params_len >= XMLCONFIG_MAX) {
            return NULL;
        }
        ngx_memcpy(ctx->cmd.options, parts[0], params_len);
        ctx->cmd.options[params_len] = '\0';

        if (ngx_http_tile_parse_int(parts[1], ngx_strlen(parts[1]), &z)
            != NGX_OK
            || ngx_http_tile_parse_int(parts[2], ngx_strlen(parts[2]), &x)
               != NGX_OK)
        {
            return NULL;
        }

        y_str = parts[3];

        if (n_parts == 5) {
            if (ngx_strcmp((char *) parts[4], "status") == 0) {
                ctx->endpoint = NGX_TILE_EP_STATUS;
            } else if (ngx_strcmp((char *) parts[4], "dirty") == 0) {
                ctx->endpoint = NGX_TILE_EP_DIRTY;
            } else {
                return NULL;
            }
        }
        /* n_parts 4 or 6: serve (upstream else-branch) */
    } else {
        /* /z/x/y.ext[/status|/dirty] */
        if (n_parts < 3) {
            return NULL;
        }

        ctx->cmd.ver = 2;
        ctx->cmd.options[0] = '\0';

        if (ngx_http_tile_parse_int(parts[0], ngx_strlen(parts[0]), &z)
            != NGX_OK
            || ngx_http_tile_parse_int(parts[1], ngx_strlen(parts[1]), &x)
               != NGX_OK)
        {
            return NULL;
        }

        y_str = parts[2];

        if (n_parts == 4) {
            if (ngx_strcmp((char *) parts[3], "status") == 0) {
                ctx->endpoint = NGX_TILE_EP_STATUS;
            } else if (ngx_strcmp((char *) parts[3], "dirty") == 0) {
                ctx->endpoint = NGX_TILE_EP_DIRTY;
            } else {
                return NULL;
            }
        }
        /* n_parts 3, 5 or 6: serve (upstream else-branch) */
    }

    /* strip the trailing ".<extension>" from the y part */
    y_len = ngx_strlen(y_str);
    dot = (u_char *) ngx_strlchr((u_char *) y_str, y_str + y_len, '.');

    if (dot == NULL) {
        return NULL;
    }

    *dot = '\0';
    y_len = dot - y_str;

    if (ngx_strlen(dot + 1) != ext_len
        || ngx_memcmp(dot + 1, layer->file_extension.data, ext_len) != 0)
    {
        /* wrong file extension: not a tile request for this layer */
        return NULL;
    }

    if (ngx_http_tile_parse_int(y_str, y_len, &y) != NGX_OK) {
        return NULL;
    }

    /* zoom range */
    oob = (z < layer->minzoom || z > layer->maxzoom);

    if (!oob) {
        /* valid x/y for tiles are 0 .. 2^zoom-1 */
        limit = (ngx_uint_t) 1 << z;
        oob = (x < 0 || (ngx_uint_t) x > (limit * layer->aspect_x - 1)
               || y < 0 || (ngx_uint_t) y > (limit * layer->aspect_y - 1));
    }

    if (oob) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "tile: request for %V was outside of allowed bounds",
                      &layer->xmlname);
        /*
         * mod_tile sleeps CLIENT_PENALTY here; nginx responds immediately
         * and client-side throttling is left to limit_req.
         */
        ctx->oob = 1;
        return ctx;
    }

    ctx->cmd.x = x;
    ctx->cmd.y = y;
    ctx->cmd.z = z;

    i = ngx_min(layer->xmlname.len, XMLCONFIG_MAX - 1);
    ngx_memcpy(ctx->cmd.xmlname, layer->xmlname.data, i);
    ctx->cmd.xmlname[i] = '\0';

    i = ngx_min(layer->mime_type.len, XMLCONFIG_MAX - 1);
    ngx_memcpy(ctx->cmd.mimetype, layer->mime_type.data, i);
    ctx->cmd.mimetype[i] = '\0';

    return ctx;
}

/* ------------------------------------------------------------------ */
/* preaccess-phase matcher                                             */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_matcher(ngx_http_request_t *r)
{
    ngx_http_tile_loc_conf_t *tlcf;
    ngx_http_tile_ctx_t      *ctx;
    ngx_http_tile_layer_t    *layer = NULL;
    const u_char             *rest = NULL;
    size_t                    rest_len = 0;
    ngx_uint_t                i;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);

    if (tlcf->stats_page) {
        ngx_http_set_ctx(r, NULL, ngx_http_tile_module);
        r->content_handler = ngx_http_tile_stats_page;
        return NGX_OK;
    }

    if (tlcf->metrics_page) {
        ngx_http_set_ctx(r, NULL, ngx_http_tile_module);
        r->content_handler = ngx_http_tile_metrics_page;
        return NGX_OK;
    }

    if (tlcf->layer.xmlname.len != 0) {
        /* location-scoped layer: nginx already routed by prefix */
        layer = &tlcf->layer;
        rest = r->uri.data + layer->baseuri.len;
        rest_len = r->uri.len - layer->baseuri.len;
    } else if (tlcf->imported_layers != NULL) {
        /* server-scoped layers imported from renderd.conf */
        ngx_http_tile_layer_t *entries = tlcf->imported_layers->elts;

        for (i = 0; i < tlcf->imported_layers->nelts; i++) {
            if (r->uri.len >= entries[i].baseuri.len
                && ngx_memcmp(r->uri.data, entries[i].baseuri.data,
                              entries[i].baseuri.len) == 0)
            {
                layer = &entries[i];
                rest = r->uri.data + layer->baseuri.len;
                rest_len = r->uri.len - layer->baseuri.len;
                break;
            }
        }
    }

    if (layer == NULL) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_tile_parse_uri(r, layer, rest, rest_len);

    if (ctx != NULL && ctx->oob) {
        return NGX_HTTP_NOT_FOUND;
    }

    if (ctx == NULL) {
        /* not a tile URI: fall through to whatever handles this location */
        return NGX_DECLINED;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_tile_module);
    r->content_handler = ngx_http_tile_content;

    return NGX_OK;
}

/* ------------------------------------------------------------------ */
/* tile state                                                          */
/* ------------------------------------------------------------------ */

ngx_tile_state_e
ngx_http_tile_stat_state(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx)
{
    struct stat_info st;

    st = ctx->store->tile_stat(ctx->store, (const char *) ctx->cmd.xmlname,
                               (const char *) ctx->cmd.options, ctx->cmd.x,
                               ctx->cmd.y, ctx->cmd.z);

    return ngx_http_tile_classify(r, ctx, st);
}

static ngx_tile_state_e
ngx_http_tile_classify(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx,
    struct stat_info st)
{
    ngx_http_tile_loc_conf_t *tlcf;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);

    ctx->mtime = st.mtime;
    ctx->atime = st.atime;

    if (st.size < 0) {
        return ngx_tile_missing;
    }

    if (st.expired) {
        if ((long) (r->start_sec - st.mtime) * 1000
            >= (long) tlcf->very_old_threshold)
        {
            return ngx_tile_very_old;
        }

        return ngx_tile_old;
    }

    return ngx_tile_current;
}

/* ------------------------------------------------------------------ */
/* post-render state check (resumes suspended requests)                */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_check_state(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx,
    ngx_int_t render_rc)
{
    ngx_http_tile_main_conf_t *mcf;
    ngx_tile_state_e           previous = ctx->tile_state;
    ngx_int_t                  rc;

    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    ctx->tile_state = ngx_http_tile_stat_state(r, ctx);

    rc = ngx_http_tile_send_response(r, ctx);

    /* fresh counters, port of the storage_hook tail: cmdDone ->
     * FRESH_RENDER regardless of the previous state; a timeout or notDone
     * keeps the stale-served counter for old/veryold tiles */
    if (render_rc == NGX_TILE_CONN_DONE) {
        ctx->fresh_code = NGX_TILE_FRESH_RENDER;
    } else if (previous == ngx_tile_old) {
        ctx->fresh_code = NGX_TILE_OLD_RENDER;
    } else if (previous == ngx_tile_very_old) {
        ctx->fresh_code = NGX_TILE_VERYOLD_RENDER;
    } else {
        ctx->fresh_code = NGX_TILE_FRESH;
    }

    ngx_http_tile_stats_fresh(mcf, ctx->fresh_code);

    return rc;
}

/* ------------------------------------------------------------------ */
/* content handler                                                     */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_content(ngx_http_request_t *r)
{
    ngx_http_tile_loc_conf_t  *tlcf;
    ngx_http_tile_main_conf_t *mcf;
    ngx_http_tile_ctx_t       *ctx;
    ngx_str_t                  store_uri;

    ctx = ngx_http_get_module_ctx(r, ngx_http_tile_module);
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);
    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    if (ngx_http_discard_request_body(r) != NGX_OK) {
        return NGX_OK;
    }

    if (ctx->endpoint == NGX_TILE_EP_JSON) {
        return ngx_http_tile_output_tilejson(r, ctx);
    }

    /* resolve the storage backend */
    store_uri = ctx->layer->store_uri.len ? ctx->layer->store_uri
                                          : tlcf->tile_dir;

    ctx->store = ngx_http_tile_get_store(mcf, &store_uri, r->connection->log);
    if (ctx->store == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ctx->endpoint == NGX_TILE_EP_STATUS) {
        return ngx_http_tile_endpoint_status(r, ctx);
    }

    if (ctx->endpoint == NGX_TILE_EP_DIRTY) {
        return ngx_http_tile_endpoint_dirty(r, ctx);
    }

    return ngx_http_tile_serve_flow(r, ctx);
}

/* ------------------------------------------------------------------ */
/* serving flow (tile_storage_hook + tile_handler_serve logic)         */
/* ------------------------------------------------------------------ */

static ngx_int_t
ngx_http_tile_serve_flow(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx)
{
    ngx_http_tile_loc_conf_t *tlcf;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);

    ctx->head_only = (r->method == NGX_HTTP_HEAD);

    /* stage 3: offload the (possibly blocking) stat to the thread pool */
    if (tlcf->thread_pool != NULL && !ctx->stat_done) {
        return ngx_http_tile_thread_stat(r, ctx);
    }

    if (!ctx->stat_done) {
        struct stat_info st = ctx->store->tile_stat(
            ctx->store, (const char *) ctx->cmd.xmlname,
            (const char *) ctx->cmd.options, ctx->cmd.x, ctx->cmd.y, ctx->cmd.z);
        ctx->st_result = st;
    }

    return ngx_http_tile_serve_flow_after_stat(r, ctx);
}

ngx_int_t
ngx_http_tile_serve_flow_after_stat(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx)
{
    ngx_http_tile_loc_conf_t *tlcf;
    ngx_http_tile_main_conf_t *mcf;
    struct protocol           dirty_cmd;
    double                    avg;
    ngx_int_t                 rc, priority;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);
    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);

    ctx->stat_done = 1;
    ctx->tile_state = ngx_http_tile_classify(r, ctx, ctx->st_result);

    /* per-IP throttle, port of the storage_hook delaypool gate */
    if (tlcf->throttle_enabled && mcf->delaypool != NULL
        && !ngx_http_tile_delay_allowed(r, tlcf, ctx->tile_state))
    {
        ngx_http_tile_stats_response(r, ctx, NGX_HTTP_SERVICE_UNAVAILABLE);
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    switch (ctx->tile_state) {
    case ngx_tile_current:
        ctx->fresh_code = NGX_TILE_FRESH;
        ngx_http_tile_stats_fresh(mcf, ctx->fresh_code);
        return ngx_http_tile_send_response(r, ctx);

    case ngx_tile_old:
    case ngx_tile_very_old:

        if (tlcf->enable_bulk_mode) {
            /* bulk mode never renders on demand */
            ctx->fresh_code = (ctx->tile_state == ngx_tile_old)
                              ? NGX_TILE_OLD : NGX_TILE_VERYOLD;
            ngx_http_tile_stats_fresh(mcf, ctx->fresh_code);
            return ngx_http_tile_send_response(r, ctx);
        }

        avg = get_load_avg();

        if (avg > tlcf->max_load_old) {
            /* too much load to re-render now: mark dirty, serve stale */
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                          "tile: load %f greater than max_load_old %d, "
                          "marking dirty and delivering from cache", avg,
                          (int) tlcf->max_load_old);
            dirty_cmd = ctx->cmd;
            ngx_http_tile_dirty_submit_detached(r, tlcf, &dirty_cmd);
            ctx->fresh_code = (ctx->tile_state == ngx_tile_old)
                              ? NGX_TILE_OLD : NGX_TILE_VERYOLD;
            ngx_http_tile_stats_fresh(mcf, ctx->fresh_code);
            return ngx_http_tile_send_response(r, ctx);
        }

        ctx->fresh_code = (ctx->tile_state == ngx_tile_old)
                          ? NGX_TILE_OLD_RENDER : NGX_TILE_VERYOLD_RENDER;
        break;

    case ngx_tile_missing:

        avg = get_load_avg();

        if (avg > tlcf->max_load_missing) {
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                          "tile: load %f greater than max_load_missing %d, "
                          "returning not found", avg,
                          (int) tlcf->max_load_missing);
            dirty_cmd = ctx->cmd;
            ngx_http_tile_dirty_submit_detached(r, tlcf, &dirty_cmd);
            ngx_http_tile_stats_response(r, ctx, NGX_HTTP_NOT_FOUND);
            return NGX_HTTP_NOT_FOUND;
        }

        break;

    default:
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "tile: unexpected tileState %d", (int) ctx->tile_state);
        ngx_http_tile_stats_response(r, ctx,
                                     NGX_HTTP_INTERNAL_SERVER_ERROR);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* on-demand render needed: suspend the request on the async engine */
    priority = (ctx->tile_state == ngx_tile_missing) ? 3
               : (ctx->tile_state == ngx_tile_old)   ? 1 : 2;

    r->main->count++;

    rc = ngx_http_tile_render_submit(r, ctx, tlcf, priority);

    if (rc != NGX_OK) {
        /* renderd unreachable: same outcome as a render timeout */
        r->main->count--;

        if (ctx->tile_state == ngx_tile_missing) {
            ngx_http_tile_stats_response(r, ctx, NGX_HTTP_NOT_FOUND);
            return NGX_HTTP_NOT_FOUND;
        }

        /* upstream still counts the stale-served old/veryold tile */
        ctx->fresh_code = (ctx->tile_state == ngx_tile_old)
                          ? NGX_TILE_OLD_RENDER : NGX_TILE_VERYOLD_RENDER;
        ngx_http_tile_stats_fresh(mcf, ctx->fresh_code);
        return ngx_http_tile_send_response(r, ctx);
    }

    return NGX_DONE;
}
