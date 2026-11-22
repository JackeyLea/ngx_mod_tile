/*
 * ngx_mod_tile - configuration declarations.
 */

#ifndef NGX_TILE_CONFIG_H
#define NGX_TILE_CONFIG_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"

#define NGX_TILE_INILINE_MAX 256 /* INILINE_MAX from upstream renderd_config.h */

extern ngx_command_t ngx_http_tile_commands[];

void *ngx_http_tile_create_loc_conf(ngx_conf_t *cf);
char *ngx_http_tile_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

#endif /* NGX_TILE_CONFIG_H */
