/*
 * ngx_mod_tile - per-IP token-bucket throttling in shared memory (stage 2).
 *
 * Port of mod_tile's delay_allowed() / delaypool. Differences documented in
 * README:
 *   - buckets refill lazily per slot (per-client timestamps) instead of the
 *     upstream global fill-up pass over all 100057 slots;
 *   - no trylock-spin / sleep(3): the slab mutex is held only for the O(1)
 *     accounting, and an empty bucket denies immediately (503).
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <arpa/inet.h>
#include <string.h>

#include "ngx_mod_tile.h"
#include "tile_delaypool.h"
#include "tile_response.h"

static ngx_int_t ngx_http_tile_delaypool_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);

ngx_int_t
ngx_http_tile_delaypool_init_zone_conf(ngx_conf_t *cf,
    ngx_http_tile_main_conf_t *mcf)
{
    ngx_str_t  name = ngx_string(NGX_TILE_DELAYPOOL_ZONE_NAME);
    size_t     size;
    ngx_str_t  sz;

    sz.data = mcf->throttle_zone_size_str.data;
    sz.len = mcf->throttle_zone_size_str.len;

    size = (size_t) ngx_parse_size(&sz);
    if (size == (size_t) NGX_ERROR
        || size < NGX_TILE_DELAYPOOL_SIZE * sizeof(ngx_tile_delay_entry_t)
                  + sizeof(ngx_tile_delaypool_shm_t))
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "tile_throttling_zone: too small for %ui slots, "
                           "need at least %uz",
                           (ngx_uint_t) NGX_TILE_DELAYPOOL_SIZE,
                           NGX_TILE_DELAYPOOL_SIZE
                           * sizeof(ngx_tile_delay_entry_t)
                           + sizeof(ngx_tile_delaypool_shm_t));
        return NGX_ERROR;
    }

    mcf->throttle_zone = ngx_shared_memory_add(cf, &name, size,
                                               &ngx_http_tile_module);
    if (mcf->throttle_zone == NULL) {
        return NGX_ERROR;
    }

    mcf->throttle_zone->init = ngx_http_tile_delaypool_init_zone;
    mcf->throttle_zone->data = mcf;

    return NGX_OK;
}

static ngx_int_t
ngx_http_tile_delaypool_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_tile_main_conf_t  *omcf = data;
    ngx_http_tile_main_conf_t  *mcf = shm_zone->data;
    ngx_slab_pool_t            *shpool;
    size_t                       size;
    u_char                      *p;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    /* slots keep their buckets across reloads; whitelist re-populated */
    if (omcf != NULL && omcf->delaypool != NULL) {
        mcf->delaypool = omcf->delaypool;
        mcf->delaypool_slab = omcf->delaypool_slab;
    } else {
        size = NGX_TILE_DELAYPOOL_SIZE * sizeof(ngx_tile_delay_entry_t)
               + sizeof(ngx_tile_delaypool_shm_t);

        ngx_shmtx_lock(&shpool->mutex);

        p = ngx_slab_alloc_locked(shpool, size);

        ngx_shmtx_unlock(&shpool->mutex);

        if (p == NULL) {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "tile: failed to allocate the delaypool array");
            return NGX_ERROR;
        }

        ngx_memzero(p, size);

        mcf->delaypool = (ngx_tile_delaypool_shm_t *) p;
        mcf->delaypool->users = (ngx_tile_delay_entry_t *)
            (p + sizeof(ngx_tile_delaypool_shm_t));
        mcf->delaypool_slab = shpool;
    }

    return NGX_OK;
}

/* resolve the client IP string honouring tile_throttling_xforward;
 * the returned string is pool-allocated */
static ngx_str_t *
delaypool_client_ip(ngx_http_request_t *r, ngx_http_tile_loc_conf_t *tlcf)
{
    ngx_str_t  *xff, *out;
    u_char     *p, *start, *end;

    if (tlcf->throttle_xforward > 0) {
        xff = ngx_http_tile_get_header(r, (u_char *) "X-Forwarded-For");

        if (xff != NULL && xff->len > 0) {
            out = ngx_palloc(r->pool, sizeof(ngx_str_t));
            if (out == NULL) {
                return &r->connection->addr_text;
            }

            p = xff->data;
            end = xff->data + xff->len;

            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }
            start = p;

            if (tlcf->throttle_xforward == 1) {
                /* first entry */
                while (p < end && *p != ',') {
                    p++;
                }
                while (p > start && (p[-1] == ' ' || p[-1] == '\t')) {
                    p--;
                }
                out->data = start;
                out->len = p - start;
                return out;
            }

            /* mode 2: last entry (the trusted proxy closest to us) */
            p = end - 1;
            while (p > xff->data && (*p == ' ' || *p == '\t' || *p == ',')) {
                p--;
            }
            end = p + 1;
            while (p > xff->data && *p != ',') {
                p--;
            }
            start = (p == xff->data && *p != ',') ? p : p + 1;
            while (start < end && (*start == ' ' || *start == '\t')) {
                start++;
            }
            out->data = start;
            out->len = end - start;
            return out;
        }
    }

    return &r->connection->addr_text;
}

/*
 * Port of delay_allowed(): 1 = allowed, 0 = over the limit.
 * state ngx_tile_missing additionally consumes a render token.
 */
ngx_int_t
ngx_http_tile_delay_allowed(ngx_http_request_t *r,
    ngx_http_tile_loc_conf_t *tlcf, ngx_tile_state_e state)
{
    ngx_http_tile_main_conf_t *mcf;
    ngx_tile_delaypool_shm_t  *dp;
    ngx_tile_delay_entry_t    *e;
    ngx_str_t                 *ip_str;
    struct in_addr             sin_addr;
    struct in6_addr            ip;
    uint32_t                   words[4], s_addr;
    uint32_t                   hashkey;
    ngx_msec_t                 now, tiles_topup, render_topup;
    ngx_int_t                  delay = 0;
    ngx_uint_t                 i, whitelisted = 0;

    mcf = ngx_http_get_module_main_conf(r, ngx_http_tile_module);
    dp = mcf->delaypool;

    if (dp == NULL) {
        return 1;
    }

    ip_str = delaypool_client_ip(r, tlcf);

    {
        char ipbuf[NGX_INET6_ADDRSTRLEN + 1];

        if (ip_str->len == 0 || ip_str->len >= sizeof(ipbuf)) {
            return 0;
        }
        ngx_memcpy(ipbuf, ip_str->data, ip_str->len);
        ipbuf[ip_str->len] = '\0';

        if (inet_pton(AF_INET, ipbuf, &sin_addr) == 1) {
            /* v4 mapped into v6, like Apache's delaypool */
            ngx_memzero(ip.s6_addr, 16);
            ngx_memcpy(&ip.s6_addr[12], &sin_addr.s_addr, 4);
        } else if (inet_pton(AF_INET6, ipbuf, &ip) != 1) {
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                          "tile: throttle cannot parse client ip \"%V\"",
                          ip_str);
            return 0;
        }
    }

    ngx_memcpy(words, ip.s6_addr, 16);
    s_addr = words[3]; /* v4-mapped: last word is the v4 address */

    /* whitelist is configuration (per server), not runtime state */
    if (tlcf->throttle_whitelist != NULL) {
        for (i = 0; i < tlcf->throttle_whitelist->nelts
                    && i < NGX_TILE_DELAYPOOL_WHITELIST_SIZE; i++)
        {
            if (((in_addr_t *) tlcf->throttle_whitelist->elts)[i] == s_addr) {
                whitelisted = 1;
                break;
            }
        }
    }

    if (whitelisted) {
        return 1;
    }

    hashkey = (words[0] ^ words[1] ^ words[2] ^ words[3])
              % NGX_TILE_DELAYPOOL_SIZE;

    now = ngx_current_msec;

    ngx_shmtx_lock(&mcf->delaypool_slab->mutex);

    e = &dp->users[hashkey];

    if (ngx_memcmp(e->ip.s6_addr, ip.s6_addr, 16) != 0) {
        /* new client or a hash collision takes over the slot, full buckets */
        ngx_memcpy(e->ip.s6_addr, ip.s6_addr, 16);
        e->available_tiles = tlcf->throttle_tile_bucket;
        e->available_render_req = tlcf->throttle_render_bucket;
        e->last_tile_topup = now;
        e->last_render_topup = now;
    } else {
        /* lazy refill of this slot only */
        if (now > e->last_tile_topup) {
            tiles_topup = (now - e->last_tile_topup) / tlcf->throttle_tile_interval;
            if (tiles_topup > 0) {
                e->last_tile_topup += tiles_topup * tlcf->throttle_tile_interval;
                e->available_tiles += tiles_topup;
                if (e->available_tiles > tlcf->throttle_tile_bucket) {
                    e->available_tiles = tlcf->throttle_tile_bucket;
                }
            }
        }

        if (now > e->last_render_topup) {
            render_topup = (now - e->last_render_topup)
                           / tlcf->throttle_render_interval;
            if (render_topup > 0) {
                e->last_render_topup += render_topup
                                        * tlcf->throttle_render_interval;
                e->available_render_req += render_topup;
                if (e->available_render_req > tlcf->throttle_render_bucket) {
                    e->available_render_req = tlcf->throttle_render_bucket;
                }
            }
        }
    }

    if (e->available_tiles > 0) {
        e->available_tiles--;
    } else {
        delay = 1;
    }

    if (state == ngx_tile_missing) {
        if (e->available_render_req > 0) {
            e->available_render_req--;
        } else {
            delay = 2;
        }
    }

    ngx_shmtx_unlock(&mcf->delaypool_slab->mutex);

    if (delay) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "tile: Delaypool: client %V has hit its limits (%i)",
                      ip_str, delay);
        return 0;
    }

    return 1;
}
