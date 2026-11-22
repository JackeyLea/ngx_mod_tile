/*
 * ngx_mod_tile - per-worker storage backend cache.
 *
 * Storage backends are instantiated lazily per worker (nginx workers are
 * single-threaded, so no locking is needed) and closed at worker exit.
 * This mirrors mod_tile's per-thread backend cache in get_storage_backend().
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"
#include "tile_store.h"

struct storage_backend *
ngx_http_tile_get_store(ngx_http_tile_main_conf_t *mcf, ngx_str_t *store_uri,
    ngx_log_t *log)
{
    ngx_uint_t  i;
    u_char     *nul_uri;

    if (store_uri->len == 0) {
        return NULL;
    }

    for (i = 0; i < mcf->stores->nelts; i++) {
        ngx_http_tile_store_t *cached = (ngx_http_tile_store_t *)
            mcf->stores->elts + i;

        if (cached->uri.len == store_uri->len
            && ngx_memcmp(cached->uri.data, store_uri->data, store_uri->len) == 0)
        {
            return cached->store;
        }
    }

    /* init_storage_backend() wants a NUL-terminated options string */
    nul_uri = ngx_pnalloc(mcf->stores->pool, store_uri->len + 1);
    if (nul_uri == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "tile: failed to allocate store URI copy");
        return NULL;
    }
    ngx_memcpy(nul_uri, store_uri->data, store_uri->len);
    nul_uri[store_uri->len] = '\0';

    {
        ngx_http_tile_store_t *entry = ngx_array_push(mcf->stores);
        struct storage_backend   *store;

        if (entry == NULL) {
            return NULL;
        }

        store = init_storage_backend((const char *) nul_uri);
        if (store == NULL || store->storage_ctx == NULL) {
            /* pop the unused entry again */
            mcf->stores->nelts--;
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "tile: failed to initialise storage backend for "
                          "\"%V\"", store_uri);
            return NULL;
        }

        entry->uri.data = nul_uri;
        entry->uri.len = store_uri->len;
        entry->store = store;

        return store;
    }
}

void
ngx_http_tile_close_stores(ngx_http_tile_main_conf_t *mcf)
{
    ngx_uint_t  i;

    for (i = 0; i < mcf->stores->nelts; i++) {
        ngx_http_tile_store_t *entry = (ngx_http_tile_store_t *)
            mcf->stores->elts + i;

        if (entry->store != NULL) {
            entry->store->close_storage(entry->store);
            free(entry->store);
            entry->store = NULL;
        }
    }

    mcf->stores->nelts = 0;
}
