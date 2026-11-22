/*
 * ngx_mod_tile - the /status, /dirty and tile-layer.json endpoints.
 *
 * Byte-exact ports of tile_handler_status, tile_handler_dirty and
 * tile_handler_json, including the upstream response strings and the
 * tab-indented TileJSON 2.0.0 layout.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"
#include "tile_response.h"
#include "tile_serve.h"
#include <ngx_md5.h>

#define NGX_TILE_TILEJSON_MAXAGE (7 * 24 * 60 * 60)

/* ------------------------------------------------------------------ */
/* /status (tile_handler_status)                                       */
/* ------------------------------------------------------------------ */

static char *
tile_ctime(ngx_pool_t *pool, time_t t)
{
    char  *p;

    /* apr_ctime format == ctime(): "Www Mmm dd hh:mm:ss yyyy\n" (24+2) */
    p = ngx_pcalloc(pool, 64);
    if (p == NULL) {
        return NULL;
    }

    if (ctime_r(&t, p) == NULL) {
        ngx_memcpy(p, "unknown", 8);
    }

    return p;
}

ngx_int_t
ngx_http_tile_endpoint_status(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx)
{
    ngx_http_tile_loc_conf_t *tlcf;
    u_char                     storage_id[PATH_MAX];
    char                      *mtime_str, *atime_str, *body;
    const char                *id;
    size_t                     size;
    u_char                    *p;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);

    if (!tlcf->enable_status_url) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "tile: /status URL is not enabled");
        return NGX_HTTP_NOT_FOUND;
    }

    ctx->tile_state = ngx_http_tile_stat_state(r, ctx);

    id = ctx->store->tile_storage_id(ctx->store,
         (const char *) ctx->cmd.xmlname, (const char *) ctx->cmd.options,
         ctx->cmd.x, ctx->cmd.y, ctx->cmd.z, (char *) storage_id);

    if (ctx->tile_state == ngx_tile_missing) {
        size_t msize = ngx_strlen(
            "No tile could not be found at storage location: \n") + ngx_strlen(id) + 1;
        char *mbody = ngx_pcalloc(r->pool, msize);
        if (mbody == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        ngx_snprintf((u_char *) mbody, msize,
            "No tile could not be found at storage location: %s\n", id);
        return ngx_http_tile_send_text(r, NGX_HTTP_OK, mbody);
    }

    mtime_str = tile_ctime(r->pool, ctx->mtime);
    atime_str = tile_ctime(r->pool, ctx->atime);
    if (mtime_str == NULL || atime_str == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    size = 1024 + ngx_strlen(id) + ngx_strlen(mtime_str)
           + ngx_strlen(atime_str);

    body = ngx_pcalloc(r->pool, size);
    if (body == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_snprintf((u_char *) body, size,
        "Tile is %s. Last rendered at %s. Last accessed at %s. Stored in %s\n"
        "\n(Dates might not be accurate. Rendering time might be reset to an "
        "old date for tile expiry. Access times might not be updated on all "
        "file systems)\n",
        (ctx->tile_state == ngx_tile_old
         || ctx->tile_state == ngx_tile_very_old)
            ? "due to be rendered" : "clean",
        mtime_str, atime_str, id);

    *p = '\0';

    return ngx_http_tile_send_text(r, NGX_HTTP_OK, body);
}

/* ------------------------------------------------------------------ */
/* /dirty (tile_handler_dirty)                                         */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_endpoint_dirty(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx)
{
    ngx_http_tile_loc_conf_t *tlcf;
    struct protocol           cmd;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);

    if (!tlcf->enable_dirty_url) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "tile: /dirty URL is not enabled");
        return NGX_HTTP_NOT_FOUND;
    }

    if (tlcf->enable_bulk_mode) {
        return ngx_http_tile_send_text(r, NGX_HTTP_OK, "");
    }

    cmd = ctx->cmd;

    /* fire-and-forget: the submission outlives this request */
    ngx_http_tile_dirty_submit_detached(r, tlcf, &cmd);

    return ngx_http_tile_send_text(r, NGX_HTTP_OK,
        "Tile submitted for rendering\n");
}

/* ------------------------------------------------------------------ */
/* tile-layer.json (tile_handler_json)                                 */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_output_tilejson(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx)
{
    ngx_http_tile_layer_t    *layer;
    u_char                   *buf, *p;
    size_t                    size = 8 * 1024;
    ngx_str_t                *hostname, default_host;
    ngx_uint_t                i;
    ngx_md5_t                 md5;
    u_char                    digest[16], hex[32], etag_buf[34];
    ngx_table_elt_t          *h;
    u_char                   *e;
    u_char                    expires_buf[64];
    time_t                    now_sec;

    layer = ctx->layer;

    if (layer->cors.len != 0) {
        ngx_int_t rc = ngx_http_tile_cors(r, &layer->cors);

        if (rc == NGX_TILE_CORS_PREFLIGHT) {
            r->headers_out.status = NGX_HTTP_OK;
            r->headers_out.content_length_n = 0;

            if (ngx_http_send_header(r) != NGX_OK) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            return ngx_http_send_special(r, NGX_HTTP_LAST);
        }

        if (rc != NGX_TILE_CORS_CONTINUE) {
            return rc;
        }
    }

    buf = ngx_pcalloc(r->pool, size);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_snprintf(buf, size,
        "{\n"
        "\t\"tilejson\": \"2.0.0\",\n"
        "\t\"schema\": \"xyz\",\n"
        "\t\"name\": \"%V\",\n"
        "\t\"description\": \"%V\",\n"
        "\t\"attribution\": \"%V\",\n"
        "\t\"minzoom\": %d,\n"
        "\t\"maxzoom\": %d,\n"
        "\t\"tiles\": [\n",
        &layer->xmlname, &layer->description, &layer->attribution,
        (int) layer->minzoom, (int) layer->maxzoom);

    if (layer->hostnames != NULL && layer->hostnames->nelts > 0) {
        ngx_str_t *hosts = layer->hostnames->elts;

        for (i = 0; i < layer->hostnames->nelts; i++) {
            hostname = &hosts[i];
            p = ngx_snprintf(p, buf + size - p,
                "\t\t\"%V%V{z}/{x}/{y}.%V\"%s\n",
                hostname, &layer->baseuri, &layer->file_extension,
                (i + 1 < layer->hostnames->nelts) ? "," : "");
        }
    } else {
        if (ngx_http_tile_hostname_string(r, &default_host) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        p = ngx_snprintf(p, buf + size - p,
            "\t\t\"%V%V{z}/{x}/{y}.%V\"\n",
            &default_host, &layer->baseuri, &layer->file_extension);
    }

    p = ngx_snprintf(p, buf + size - p, "\t]\n}\n");

    /* ETag of the JSON body */
    ngx_md5_init(&md5);
    ngx_md5_update(&md5, buf, p - buf);
    ngx_md5_final(digest, &md5);
    ngx_hex_dump(hex, digest, 16);

    etag_buf[0] = '"';
    ngx_memcpy(etag_buf + 1, hex, 32);
    etag_buf[33] = '"';

    h = ngx_list_push(&r->headers_out.headers);
    if (h != NULL) {
        h->hash = 1;
        ngx_str_set(&h->key, "ETag");
        h->value.data = ngx_pnalloc(r->pool, 34);
        if (h->value.data != NULL) {
            ngx_memcpy(h->value.data, etag_buf, 34);
            h->value.len = 34;
        }
    }

    ngx_str_set(&r->headers_out.content_type, "application/json");
    r->headers_out.content_type_len = sizeof("application/json") - 1;
    r->headers_out.content_length_n = p - buf;
    r->headers_out.status = NGX_HTTP_OK;

    /* Cache-Control: max-age=604800 + Expires, one week */
    h = ngx_list_push(&r->headers_out.headers);
    if (h != NULL) {
        h->hash = 1;
        ngx_str_set(&h->key, "Cache-Control");
        h->value.data = ngx_pnalloc(r->pool, 32);
        if (h->value.data != NULL) {
            e = ngx_snprintf(h->value.data, 31, "max-age=%d",
                             NGX_TILE_TILEJSON_MAXAGE);
            h->value.len = e - h->value.data;
        }
    }

    now_sec = r->start_sec;

    e = ngx_http_cookie_time(expires_buf, now_sec + NGX_TILE_TILEJSON_MAXAGE);
    h = ngx_list_push(&r->headers_out.headers);
    if (h != NULL) {
        h->hash = 1;
        ngx_str_set(&h->key, "Expires");
        h->value.data = ngx_pnalloc(r->pool, ngx_strlen(e));
        if (h->value.data != NULL) {
            ngx_memcpy(h->value.data, e, ngx_strlen(e));
            h->value.len = ngx_strlen(e);
        }
    }

    if (ngx_http_send_header(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (r->header_only) {
        return NGX_OK;
    }

    {
        ngx_buf_t   *b;
        ngx_chain_t  out;

        b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
        if (b == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        b->pos = buf;
        b->last = p;
        b->memory = 1;
        b->last_buf = (r == r->main) ? 1 : 0;
        b->last_in_chain = 1;

        out.buf = b;
        out.next = NULL;

        return ngx_http_output_filter(r, &out);
    }
}
