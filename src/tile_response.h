/*
 * ngx_mod_tile - response building declarations.
 */

#ifndef NGX_TILE_RESPONSE_H
#define NGX_TILE_RESPONSE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"

/* CORS outcomes (mirrors add_cors' DONE/OK/FORBIDDEN) */
#define NGX_TILE_CORS_CONTINUE   NGX_OK
#define NGX_TILE_CORS_PREFLIGHT  NGX_DONE

ngx_str_t *ngx_http_tile_get_header(ngx_http_request_t *r, u_char *name);
ngx_int_t ngx_http_tile_hostname_string(ngx_http_request_t *r, ngx_str_t *dest);

#endif /* NGX_TILE_RESPONSE_H */
