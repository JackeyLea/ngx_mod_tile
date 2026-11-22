/*
 * ngx_mod_tile - request routing declarations.
 */

#ifndef NGX_TILE_SERVE_H
#define NGX_TILE_SERVE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"

ngx_tile_state_e ngx_http_tile_stat_state(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);

#endif /* NGX_TILE_SERVE_H */
