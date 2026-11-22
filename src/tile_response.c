/*
 * ngx_mod_tile - HTTP response construction.
 *
 * Faithful port of mod_tile's response path (tile_handler_serve +
 * add_cors + add_expiry + ETag/meets-conditions), with the MD5 ETag,
 * Cache-Control/Expires heuristic (including the random jitter and the
 * extended-cache-hostname override), CORS handling with OPTIONS
 * preflight short-circuit, and gzip Content-Encoding for METZ
 * (compressed) metatiles.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"
#include "tile_response.h"
#include <ngx_md5.h>

#define NGX_TILE_CORS_MAX_AGE 604800

static ngx_table_elt_t *ngx_http_tile_push_header(ngx_http_request_t *r,
    const char *key, const char *value);
static ngx_int_t ngx_http_tile_conditions(ngx_http_request_t *r, u_char *etag,
    size_t etag_len);
static ngx_int_t ngx_http_tile_output_body(ngx_http_request_t *r, u_char *buf,
    size_t len);

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

ngx_str_t *
ngx_http_tile_get_header(ngx_http_request_t *r, u_char *name)
{
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;
    ngx_uint_t        i;
    size_t            len = ngx_strlen(name);

    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].key.len == len
            && ngx_strncasecmp(h[i].key.data, name, len) == 0)
        {
            return &h[i].value;
        }
    }

    return NULL;
}

static ngx_table_elt_t *
ngx_http_tile_push_header(ngx_http_request_t *r, const char *key,
    const char *value)
{
    ngx_table_elt_t *h;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NULL;
    }

    h->hash = 1;
    /* key comes from a runtime string: ngx_str_set would take sizeof(char*)
       here and truncate the header name */
    h->key.len = ngx_strlen(key);
    h->key.data = (u_char *) key;

    if (value != NULL) {
        h->value.data = (u_char *) value;
        h->value.len = ngx_strlen(value);
    } else {
        h->value.data = (u_char *) "";
        h->value.len = 0;
    }

    return h;
}

/* case-insensitive substring search (needle NUL-free) */
static unsigned
tile_contains_ci(ngx_str_t *haystack, u_char *needle, size_t needle_len)
{
    size_t  i, j;

    if (haystack->len < needle_len) {
        return 0;
    }

    for (i = 0; i + needle_len <= haystack->len; i++) {
        for (j = 0; j < needle_len; j++) {
            if (ngx_tolower(haystack->data[i + j]) != ngx_tolower(needle[j])) {
                break;
            }
        }
        if (j == needle_len) {
            return 1;
        }
    }

    return 0;
}

/*
 * Port of ap_meets_conditions for the If-None-Match header only
 * (mod_tile never emits Last-Modified, so If-Modified-Since never
 * matches upstream either).
 */
static ngx_int_t
ngx_http_tile_conditions(ngx_http_request_t *r, u_char *etag, size_t etag_len)
{
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;
    ngx_uint_t        i;

    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].key.len == sizeof("If-None-Match") - 1
            && ngx_strncasecmp(h[i].key.data, (u_char *) "If-None-Match",
                               sizeof("If-None-Match") - 1) == 0)
        {
            u_char *p, *last;

            p = h[i].value.data;
            last = p + h[i].value.len;

            while (p < last) {
                u_char *tok, *tok_end;

                while (p < last && (*p == ' ' || *p == ',' || *p == '\t')) {
                    p++;
                }
                if (p >= last) {
                    break;
                }
                tok = p;
                while (p < last && *p != ',') {
                    p++;
                }
                tok_end = p;

                /* trim trailing spaces */
                while (tok_end > tok && (tok_end[-1] == ' '
                                         || tok_end[-1] == '\t'))
                {
                    tok_end--;
                }

                /* wildcard matches everything */
                if (tok_end - tok == 1 && tok[0] == '*') {
                    return NGX_HTTP_NOT_MODIFIED;
                }

                /* weak comparison: strip a W/ prefix before comparing */
                if (tok_end - tok > 2 && tok[0] == 'W' && tok[1] == '/') {
                    tok += 2;
                }

                if ((size_t) (tok_end - tok) == etag_len
                    && ngx_memcmp(tok, etag, etag_len) == 0)
                {
                    return NGX_HTTP_NOT_MODIFIED;
                }
            }
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_tile_output_body(ngx_http_request_t *r, u_char *buf, size_t len)
{
    ngx_buf_t    *b;
    ngx_chain_t   out;

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->pos = buf;
    b->last = buf + len;
    b->memory = 1;
    b->last_buf = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}

/* derive "http://<server_name>" for TileJSON when no tile_hostname set */
ngx_int_t
ngx_http_tile_hostname_string(ngx_http_request_t *r, ngx_str_t *dest)
{
    ngx_http_core_srv_conf_t  *cscf;
    size_t                      len;

    cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);

    len = sizeof("http://") - 1 + cscf->server_name.len;

    dest->data = ngx_pnalloc(r->pool, len);
    if (dest->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(dest->data, "http://", sizeof("http://") - 1);
    ngx_memcpy(dest->data + sizeof("http://") - 1, cscf->server_name.data,
               cscf->server_name.len);
    dest->len = len;

    return NGX_OK;
}

/* ------------------------------------------------------------------ */
/* CORS (port of add_cors)                                             */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_cors(ngx_http_request_t *r, ngx_str_t *cors)
{
    ngx_table_elt_t       *h;
    ngx_str_t             *origin;
    ngx_uint_t             allowed = 0;
    origin = ngx_http_tile_get_header(r, (u_char *) "Origin");

    if (origin == NULL) {
        return NGX_TILE_CORS_CONTINUE;
    }

    /* upstream matches by strcmp(cors, "*") or strstr(cors, origin) */
    if (cors->len == 1 && cors->data[0] == '*') {
        allowed = 1;
    } else {
        /* substring match of the origin inside the configured policy */
        if (cors->len >= origin->len
            && ngx_strnstr(cors->data, (char *) origin->data, cors->len)
               != NULL)
        {
            allowed = 1;
        }
    }

    if (!allowed) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "tile: Origin header %V is NOT allowed under the CORS "
                      "policy %V", origin, cors);
        return NGX_HTTP_FORBIDDEN;
    }

    h = ngx_http_tile_push_header(r, "Access-Control-Allow-Origin",
        (cors->len == 1 && cors->data[0] == '*') ? "*" : NULL);
    if (h == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (!(cors->len == 1 && cors->data[0] == '*')) {
        h->value.data = ngx_pnalloc(r->pool, origin->len);
        if (h->value.data == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        ngx_memcpy(h->value.data, origin->data, origin->len);
        h->value.len = origin->len;

        h = ngx_http_tile_push_header(r, "Vary", "Origin");
        if (h == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    /* OPTIONS preflight short-circuit */
    if (r->method == NGX_HTTP_OPTIONS
        && ngx_http_tile_get_header(r,
               (u_char *) "Access-Control-Request-Method") != NULL)
    {
        ngx_str_t *req_headers;

        req_headers = ngx_http_tile_get_header(r,
            (u_char *) "Access-Control-Request-Headers");

        if (req_headers != NULL) {
            h = ngx_http_tile_push_header(r, "Access-Control-Allow-Headers",
                "");
            if (h == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            h->value.data = ngx_pnalloc(r->pool, req_headers->len);
            if (h->value.data == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            ngx_memcpy(h->value.data, req_headers->data, req_headers->len);
            h->value.len = req_headers->len;
        }

        {
            u_char    *max_age;
            u_char    *last;
            ngx_int_t  max_age_val = NGX_TILE_CORS_MAX_AGE;

            /* response headers must live on the request pool, never on the
             * stack; nginx's %d consumes 8 bytes, hence the ngx_int_t */
            max_age = ngx_pnalloc(r->pool, 16);
            if (max_age == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            last = ngx_snprintf(max_age, 15, "%d", max_age_val);
            *last = '\0';

            h = ngx_http_tile_push_header(r, "Access-Control-Max-Age",
                (char *) max_age);
            if (h == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
        }

        return NGX_TILE_CORS_PREFLIGHT;
    }

    return NGX_TILE_CORS_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* cache expiry (port of add_expiry)                                   */
/* ------------------------------------------------------------------ */

static ngx_str_t *
ngx_http_tile_request_hostname(ngx_http_request_t *r)
{
    /* r->headers_in.server is the lowercased Host header without port */
    return &r->headers_in.server;
}

void
ngx_http_tile_add_expiry(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx)
{
    ngx_http_tile_loc_conf_t *tlcf;
    ngx_str_t                 *host;
    long int                   maxAge, minCache = 0, lastModified = 0;
    double                     holdoff;
    u_char                     buf[64];
    u_char                    *last;
    ngx_table_elt_t           *h;
    u_char                    *expires;
    u_char                    *expires_buf;
    time_t                     now_sec;

    tlcf = ngx_http_get_module_loc_conf(r, ngx_http_tile_module);
    now_sec = r->start_sec;

    /* extended-cache-hostname override: substring match, skips the clamp */
    host = ngx_http_tile_request_hostname(r);

    if (tlcf->cache_extended_hostname.len != 0 && host->len != 0
        && ngx_strnstr(host->data, (char *) tlcf->cache_extended_hostname.data,
                       host->len) != NULL)
    {
        maxAge = tlcf->cache_extended_duration;
    } else {
        if (ctx->tile_state == ngx_tile_old
            || ctx->tile_state == ngx_tile_very_old)
        {
            holdoff = (tlcf->cache_duration_dirty / 2.0)
                      * (rand() / (RAND_MAX + 1.0));
            maxAge = tlcf->cache_duration_dirty + holdoff;
        } else {
            if (ctx->cmd.z > ctx->layer->maxzoom) {
                minCache = 0;
            } else {
                minCache = tlcf->mincachetime[ctx->cmd.z];
            }

            /* time since the tile was last rendered, scaled by the factor */
            lastModified = (long int) (((double) (now_sec - ctx->mtime))
                             * tlcf->cache_duration_last_modified_factor);

            /* random jitter of 3 hours to space out cache expiry */
            holdoff = (3 * 60 * 60) * (rand() / (RAND_MAX + 1.0));

            maxAge = minCache;
            if (maxAge < lastModified) {
                maxAge = lastModified;
            }
            maxAge += holdoff;
        }

        if (maxAge > tlcf->cache_duration_max) {
            maxAge = tlcf->cache_duration_max;
        }
    }

    last = ngx_snprintf(buf, sizeof(buf) - 1, "max-age=%ld", maxAge);

    h = ngx_list_push(&r->headers_out.headers);
    if (h != NULL) {
        h->hash = 1;
        ngx_str_set(&h->key, "Cache-Control");
        h->value.data = ngx_pnalloc(r->pool, last - buf);
        if (h->value.data != NULL) {
            ngx_memcpy(h->value.data, buf, last - buf);
            h->value.len = last - buf;
        }
    }

    expires_buf = ngx_pnalloc(r->pool, 64);
    if (expires_buf == NULL) {
        return;
    }
    expires = ngx_http_cookie_time(expires_buf, now_sec + maxAge);

    h = ngx_list_push(&r->headers_out.headers);
    if (h != NULL) {
        h->hash = 1;
        ngx_str_set(&h->key, "Expires");
        h->value.data = expires_buf;
        h->value.len = ngx_strlen(expires);
    }
}

/* ------------------------------------------------------------------ */
/* tile response (port of tile_handler_serve output path)              */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_send_response(ngx_http_request_t *r, ngx_http_tile_ctx_t *ctx)
{
    u_char                    *buf;
    char                       err_msg[1024];
    int                        compressed = 0;
    ssize_t                    len;
    u_char                     digest[16], hex[32], etag_buf[34];
    ngx_md5_t                  md5;
    ngx_str_t                 *accept_enc;
    ngx_table_elt_t           *h;
    ngx_int_t                  rc;

    if (ctx->layer->cors.len != 0) {
        rc = ngx_http_tile_cors(r, &ctx->layer->cors);

        if (rc == NGX_TILE_CORS_PREFLIGHT) {
            /* empty 200 terminating the preflight exchange */
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

    buf = ngx_palloc(r->pool, MAX_SIZE);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    err_msg[0] = '\0';

    len = ctx->store->tile_read(ctx->store, (const char *) ctx->cmd.xmlname,
                                (const char *) ctx->cmd.options, ctx->cmd.x,
                                ctx->cmd.y, ctx->cmd.z, (char *) buf, MAX_SIZE,
                                &compressed, err_msg);

    if (len <= 0) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "tile: failed to read tile %s z%d x%d y%d from "
                      "storage: %s", ctx->cmd.xmlname, ctx->cmd.z,
                      ctx->cmd.x, ctx->cmd.y, err_msg);
        return NGX_HTTP_NOT_FOUND;
    }

    /* gzip-encoded metatiles (METZ magic): pass through when supported */
    if (compressed) {
        accept_enc = ngx_http_tile_get_header(r,
            (u_char *) "Accept-Encoding");

        if (accept_enc != NULL
            && tile_contains_ci(accept_enc, (u_char *) "gzip", 4))
        {
            h = ngx_http_tile_push_header(r, "Content-Encoding", "gzip");
            if (h == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
        } else {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "tile: tile data is compressed but the user agent "
                          "does not support Content-Encoding");
        }
    }

    /* MD5 ETag: identical re-renders keep client caches valid */
    ngx_md5_init(&md5);
    ngx_md5_update(&md5, buf, len);
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

    r->headers_out.content_type_len = ctx->layer->mime_type.len;
    r->headers_out.content_type = ctx->layer->mime_type;
    r->headers_out.content_length_n = len;

    ngx_http_tile_add_expiry(r, ctx);

    rc = ngx_http_tile_conditions(r, etag_buf, 34);
    if (rc != NGX_OK) {
        r->headers_out.status = rc; /* NGX_HTTP_NOT_MODIFIED */
        if (ngx_http_send_header(r) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        return NGX_OK;
    }

    r->headers_out.status = NGX_HTTP_OK;

    if (ngx_http_send_header(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (r->header_only) {
        return NGX_OK;
    }

    return ngx_http_tile_output_body(r, buf, len);
}

/* ------------------------------------------------------------------ */
/* text/plain helper (status & dirty endpoints)                        */
/* ------------------------------------------------------------------ */

ngx_int_t
ngx_http_tile_send_text(ngx_http_request_t *r, ngx_uint_t status,
    const char *body)
{
    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    r->headers_out.status = status;
    r->headers_out.content_length_n = ngx_strlen(body);

    if (ngx_http_send_header(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (r->header_only) {
        return NGX_OK;
    }

    if (ngx_strlen(body) == 0) {
        return ngx_http_send_special(r, NGX_HTTP_LAST);
    }

    return ngx_http_tile_output_body(r, (u_char *) body, ngx_strlen(body));
}
