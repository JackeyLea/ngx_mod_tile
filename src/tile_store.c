/*
 * ngx_mod_tile - per-worker / per-thread storage backend caches.
 *
 * The event loop (sync paths) uses one instance per store URI per worker;
 * thread-pool tasks use per-thread instances (pthread TLS) because the
 * network-backed clients (libmemcached, curl easy handles) are not
 * thread-safe. This mirrors Apache mod_tile's per-thread backend cache.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <pthread.h>

#include "ngx_mod_tile.h"
#include "tile_store.h"

/* ---- worker-global cache (single-threaded event loop) ---- */

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
        ngx_http_tile_store_t *cached =
            (ngx_http_tile_store_t *) mcf->stores->elts + i;

        if (cached->uri.len == store_uri->len
            && ngx_memcmp(cached->uri.data, store_uri->data, store_uri->len)
               == 0)
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
        struct storage_backend *store;

        if (entry == NULL) {
            return NULL;
        }

        store = init_storage_backend((const char *) nul_uri);
        if (store == NULL || store->storage_ctx == NULL) {
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
        ngx_http_tile_store_t *entry =
            (ngx_http_tile_store_t *) mcf->stores->elts + i;

        if (entry->store != NULL) {
            entry->store->close_storage(entry->store);
            free(entry->store);
            entry->store = NULL;
        }
    }

    mcf->stores->nelts = 0;
}

/* ---- per-thread cache for thread-pool tasks ---- */

typedef struct {
    struct storage_backend **stores;
    u_char                 **uris;
    ngx_uint_t               n;
    ngx_uint_t               cap;
} ngx_http_tile_tls_t;

static pthread_key_t  ngx_http_tile_tls_key;
static ngx_uint_t     ngx_http_tile_tls_key_init = 0;

static void
ngx_http_tile_tls_destructor(void *data)
{
    ngx_http_tile_tls_t *tls = data;
    ngx_uint_t            i;

    if (tls == NULL) {
        return;
    }

    for (i = 0; i < tls->n; i++) {
        if (tls->stores[i] != NULL) {
            tls->stores[i]->close_storage(tls->stores[i]);
            free(tls->stores[i]);
        }
        if (tls->uris[i] != NULL) {
            free(tls->uris[i]);
        }
    }

    free(tls->stores);
    free(tls->uris);
    free(tls);
}

void
ngx_http_tile_store_tls_init(void)
{
    if (!ngx_http_tile_tls_key_init) {
        if (pthread_key_create(&ngx_http_tile_tls_key,
                               ngx_http_tile_tls_destructor) == 0) {
            ngx_http_tile_tls_key_init = 1;
        }
    }
}

void
ngx_http_tile_store_tls_destroy(void)
{
    if (ngx_http_tile_tls_key_init) {
        pthread_key_delete(ngx_http_tile_tls_key);
        ngx_http_tile_tls_key_init = 0;
    }
}

struct storage_backend *
ngx_http_tile_get_store_thread(ngx_http_tile_main_conf_t *mcf,
    ngx_str_t *store_uri, ngx_log_t *log)
{
    ngx_http_tile_tls_t    *tls;
    ngx_uint_t              i;
    struct storage_backend *store;
    u_char                 *nul_uri;

    if (!ngx_http_tile_tls_key_init) {
        ngx_http_tile_store_tls_init();
        if (!ngx_http_tile_tls_key_init) {
            return NULL;
        }
    }

    tls = pthread_getspecific(ngx_http_tile_tls_key);

    if (tls == NULL) {
        tls = malloc(sizeof(ngx_http_tile_tls_t));
        if (tls == NULL) {
            return NULL;
        }

        tls->n = 0;
        tls->cap = 4;
        tls->stores = malloc(tls->cap * sizeof(struct storage_backend *));
        tls->uris = malloc(tls->cap * sizeof(u_char *));

        if (tls->stores == NULL || tls->uris == NULL
            || pthread_setspecific(ngx_http_tile_tls_key, tls) != 0)
        {
            free(tls->stores);
            free(tls->uris);
            free(tls);
            return NULL;
        }
    }

    for (i = 0; i < tls->n; i++) {
        if (ngx_strlen(tls->uris[i]) == store_uri->len
            && ngx_memcmp(tls->uris[i], store_uri->data, store_uri->len) == 0)
        {
            return tls->stores[i];
        }
    }

    if (tls->n == tls->cap) {
        tls->cap *= 2;
        tls->stores = realloc(tls->stores,
                              tls->cap * sizeof(struct storage_backend *));
        tls->uris = realloc(tls->uris, tls->cap * sizeof(u_char *));

        if (tls->stores == NULL || tls->uris == NULL) {
            return NULL;
        }
    }

    nul_uri = malloc(store_uri->len + 1);
    if (nul_uri == NULL) {
        return NULL;
    }
    ngx_memcpy(nul_uri, store_uri->data, store_uri->len);
    nul_uri[store_uri->len] = '\0';

    store = init_storage_backend((const char *) nul_uri);
    if (store == NULL || store->storage_ctx == NULL) {
        free(nul_uri);
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "tile: failed to initialise thread store for \"%V\"",
                      store_uri);
        return NULL;
    }

    tls->uris[tls->n] = nul_uri;
    tls->stores[tls->n] = store;
    tls->n++;

    return store;
}
