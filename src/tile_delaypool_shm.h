/*
 * ngx_mod_tile - shared per-IP delaypool structures (stage 2).
 *
 * Same slot count as Apache (100057); each slot carries its own refill
 * timestamps so buckets top up lazily per client instead of Apache's
 * global fill-up pass. Locking uses the slab mutex (no trylock-spin or
 * sleep(3): nginx denies immediately when a bucket is empty).
 */

#ifndef NGX_TILE_DELAYPOOL_SHM_H
#define NGX_TILE_DELAYPOOL_SHM_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <netinet/in.h>

#define NGX_TILE_DELAYPOOL_SIZE           100057
#define NGX_TILE_DELAYPOOL_WHITELIST_SIZE 13

#define NGX_TILE_DELAYPOOL_ZONE_NAME "ngx_mod_tile_throttle"
#define NGX_TILE_DELAYPOOL_ZONE_DEFAULT "8m"

typedef struct {
    struct in6_addr ip;                 /* client IP, v4 mapped to ::ffff:v4 */
    ngx_int_t       available_tiles;
    ngx_int_t       available_render_req;
    ngx_msec_t      last_tile_topup;
    ngx_msec_t      last_render_topup;
} ngx_tile_delay_entry_t;

typedef struct {
    ngx_tile_delay_entry_t *users;      /* NGX_TILE_DELAYPOOL_SIZE slots */
} ngx_tile_delaypool_shm_t;

#endif /* NGX_TILE_DELAYPOOL_SHM_H */
