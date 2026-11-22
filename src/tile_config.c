/*
 * ngx_mod_tile - configuration handling.
 *
 * Directive handlers for the tile layer configuration and the port of
 * mod_tile's renderd.conf [map] section parser (process_map_sections),
 * rewritten with iniparser lookups converted from exit() to error
 * returns so a bad file aborts nginx startup with a proper message.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <iniparser.h>
#include <string.h>
#include <stdlib.h>

#include "ngx_mod_tile.h"
#include "tile_config.h"

extern ngx_module_t ngx_http_core_module;

#define NGX_TILE_XMLCONFIGS_MAX 100  /* XMLCONFIGS_MAX from render_config.h */

static char *ngx_http_tile_layer_directive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_tile_zoom_directive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_tile_hostname_directive(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_tile_renderd_socket_directive(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_tile_factor_directive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_tile_cache_low_medium_directive(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_tile_import_config_directive(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

ngx_command_t ngx_http_tile_commands[] = {

    /* --- layer definition (location scope) --- */

    { ngx_string("tile_layer"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_tile_layer_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("tile_file_extension"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.file_extension),
      NULL },

    { ngx_string("tile_mime_type"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.mime_type),
      NULL },

    { ngx_string("tile_minzoom"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_tile_zoom_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.minzoom),
      NULL },

    { ngx_string("tile_maxzoom"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_tile_zoom_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.maxzoom),
      NULL },

    { ngx_string("tile_aspect_x"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.aspect_x),
      NULL },

    { ngx_string("tile_aspect_y"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.aspect_y),
      NULL },

    { ngx_string("tile_attribution"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.attribution),
      NULL },

    { ngx_string("tile_description"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.description),
      NULL },

    { ngx_string("tile_cors"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.cors),
      NULL },

    { ngx_string("tile_options"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.enable_options),
      NULL },

    { ngx_string("tile_hostname"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_tile_hostname_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, layer.hostnames),
      NULL },

    /* --- server-wide settings --- */

    { ngx_string("tile_renderd_socket_addr"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_tile_renderd_socket_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("tile_tile_dir"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, tile_dir),
      NULL },

    { ngx_string("tile_request_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, request_timeout),
      NULL },

    { ngx_string("tile_missing_request_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, request_timeout_priority),
      NULL },

    { ngx_string("tile_renderd_connect_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, connect_timeout),
      NULL },

    { ngx_string("tile_max_load_old"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, max_load_old),
      NULL },

    { ngx_string("tile_max_load_missing"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, max_load_missing),
      NULL },

    { ngx_string("tile_very_old_threshold"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, very_old_threshold),
      NULL },

    { ngx_string("tile_cache_last_modified_factor"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_tile_factor_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, cache_duration_last_modified_factor),
      NULL },

    { ngx_string("tile_cache_duration_dirty"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, cache_duration_dirty),
      NULL },

    { ngx_string("tile_cache_duration_max"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, cache_duration_max),
      NULL },

    { ngx_string("tile_cache_duration_minimum"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, cache_duration_minimum),
      NULL },

    { ngx_string("tile_cache_duration_low_zoom"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_tile_cache_low_medium_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("tile_cache_duration_medium_zoom"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_tile_cache_low_medium_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("tile_cache_extended_hostname"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, cache_extended_hostname),
      NULL },

    { ngx_string("tile_cache_extended_duration"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, cache_extended_duration),
      NULL },

    { ngx_string("tile_bulk_mode"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, enable_bulk_mode),
      NULL },

    { ngx_string("tile_enable_dirty_url"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, enable_dirty_url),
      NULL },

    { ngx_string("tile_enable_status_url"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_tile_loc_conf_t, enable_status_url),
      NULL },

    { ngx_string("tile_import_config"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_http_tile_import_config_directive,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    ngx_null_command
};

/* ------------------------------------------------------------------ */
/* conf create/merge                                                   */
/* ------------------------------------------------------------------ */

void *
ngx_http_tile_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_tile_loc_conf_t *tlcf;

    tlcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_tile_loc_conf_t));
    if (tlcf == NULL) {
        return NULL;
    }

    /* sentinel values: 0/-1/NULL mean "not set", merged or defaulted later */
    tlcf->layer.minzoom = NGX_CONF_UNSET;
    tlcf->layer.maxzoom = NGX_CONF_UNSET;
    tlcf->layer.aspect_x = NGX_CONF_UNSET;
    tlcf->layer.aspect_y = NGX_CONF_UNSET;
    tlcf->layer.enable_options = NGX_CONF_UNSET;

    tlcf->renderd_port = NGX_CONF_UNSET;
    tlcf->request_timeout = NGX_CONF_UNSET;
    tlcf->request_timeout_priority = NGX_CONF_UNSET;
    tlcf->connect_timeout = NGX_CONF_UNSET;
    tlcf->max_load_old = NGX_CONF_UNSET;
    tlcf->max_load_missing = NGX_CONF_UNSET;
    tlcf->very_old_threshold = NGX_CONF_UNSET;
    tlcf->cache_duration_last_modified_factor = NGX_TILE_UNSET_DOUBLE;
    tlcf->cache_duration_dirty = NGX_CONF_UNSET;
    tlcf->cache_duration_max = NGX_CONF_UNSET;
    tlcf->cache_duration_minimum = NGX_CONF_UNSET;
    tlcf->cache_duration_low_zoom = NGX_CONF_UNSET;
    tlcf->cache_duration_medium_zoom = NGX_CONF_UNSET;
    tlcf->cache_level_low_zoom = NGX_CONF_UNSET;
    tlcf->cache_level_medium_zoom = NGX_CONF_UNSET;
    tlcf->cache_extended_duration = NGX_CONF_UNSET;
    tlcf->enable_bulk_mode = NGX_CONF_UNSET;
    tlcf->enable_dirty_url = NGX_CONF_UNSET;
    tlcf->enable_status_url = NGX_CONF_UNSET;

    return tlcf;
}

char *
ngx_http_tile_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_tile_loc_conf_t *prev = parent;
    ngx_http_tile_loc_conf_t *conf = child;
    ngx_int_t                  i;

    ngx_conf_merge_str_value(conf->layer.file_extension, prev->layer.file_extension, "png");
    ngx_conf_merge_str_value(conf->layer.mime_type, prev->layer.mime_type, "image/png");
    ngx_conf_merge_value(conf->layer.minzoom, prev->layer.minzoom, 0);
    ngx_conf_merge_value(conf->layer.maxzoom, prev->layer.maxzoom, 20);
    ngx_conf_merge_value(conf->layer.aspect_x, prev->layer.aspect_x, 1);
    ngx_conf_merge_value(conf->layer.aspect_y, prev->layer.aspect_y, 1);
    ngx_conf_merge_value(conf->layer.enable_options, prev->layer.enable_options, 0);

    ngx_conf_merge_str_value(conf->renderd_socket, prev->renderd_socket,
                             "unix:/run/renderd/renderd.sock");
    ngx_conf_merge_str_value(conf->renderd_host, prev->renderd_host, "");
    ngx_conf_merge_value(conf->renderd_port, prev->renderd_port, 0);
    ngx_conf_merge_str_value(conf->socket_path, prev->socket_path, "");

    ngx_conf_merge_str_value(conf->tile_dir, prev->tile_dir,
                             "/var/cache/renderd/tiles");
    ngx_conf_merge_msec_value(conf->request_timeout, prev->request_timeout,
                              NGX_TILE_REQUEST_TIMEOUT);
    ngx_conf_merge_msec_value(conf->request_timeout_priority,
                              prev->request_timeout_priority,
                              NGX_TILE_MISSING_REQUEST_TIMEOUT);
    ngx_conf_merge_msec_value(conf->connect_timeout, prev->connect_timeout,
                              NGX_TILE_CONNECT_TIMEOUT);
    ngx_conf_merge_value(conf->max_load_old, prev->max_load_old,
                         NGX_TILE_MAX_LOAD_OLD);
    ngx_conf_merge_value(conf->max_load_missing, prev->max_load_missing,
                         NGX_TILE_MAX_LOAD_MISSING);
    ngx_conf_merge_msec_value(conf->very_old_threshold,
                              prev->very_old_threshold,
                              NGX_TILE_VERY_OLD_THRESHOLD_MS);

    if (conf->cache_duration_last_modified_factor == NGX_TILE_UNSET_DOUBLE) {
        conf->cache_duration_last_modified_factor =
            prev->cache_duration_last_modified_factor;
        if (conf->cache_duration_last_modified_factor == NGX_TILE_UNSET_DOUBLE) {
            conf->cache_duration_last_modified_factor = 0.0;
        }
    }

    ngx_conf_merge_value(conf->cache_duration_dirty, prev->cache_duration_dirty,
                         NGX_TILE_CACHE_DURATION_DIRTY);
    ngx_conf_merge_value(conf->cache_duration_max, prev->cache_duration_max,
                         NGX_TILE_CACHE_DURATION_MAX);
    ngx_conf_merge_value(conf->cache_duration_minimum,
                         prev->cache_duration_minimum,
                         NGX_TILE_CACHE_DURATION_MINIMUM);
    ngx_conf_merge_value(conf->cache_duration_low_zoom,
                         prev->cache_duration_low_zoom,
                         NGX_TILE_CACHE_DURATION_LOW);
    ngx_conf_merge_value(conf->cache_duration_medium_zoom,
                         prev->cache_duration_medium_zoom,
                         NGX_TILE_CACHE_DURATION_MEDIUM);
    ngx_conf_merge_value(conf->cache_level_low_zoom, prev->cache_level_low_zoom, 0);
    ngx_conf_merge_value(conf->cache_level_medium_zoom,
                         prev->cache_level_medium_zoom, 0);
    ngx_conf_merge_value(conf->cache_extended_duration,
                         prev->cache_extended_duration, 0);
    ngx_conf_merge_str_value(conf->cache_extended_hostname,
                             prev->cache_extended_hostname, "");

    ngx_conf_merge_value(conf->enable_bulk_mode, prev->enable_bulk_mode, 0);
    ngx_conf_merge_value(conf->enable_dirty_url, prev->enable_dirty_url, 1);
    ngx_conf_merge_value(conf->enable_status_url, prev->enable_status_url, 1);

    if (conf->imported_layers == NULL) {
        conf->imported_layers = prev->imported_layers;
    }

    /* Per-zoom minimum cache duration table (merge_tile_config equivalent) */
    for (i = 0; i <= NGX_TILE_MAX_ZOOM_SERVER; i++) {
        if (i <= conf->cache_level_low_zoom) {
            conf->mincachetime[i] = conf->cache_duration_low_zoom;
        } else if (i <= conf->cache_level_medium_zoom) {
            conf->mincachetime[i] = conf->cache_duration_medium_zoom;
        } else {
            conf->mincachetime[i] = conf->cache_duration_minimum;
        }
    }

    return NGX_CONF_OK;
}

/* ------------------------------------------------------------------ */
/* directive handlers                                                  */
/* ------------------------------------------------------------------ */

static char *
ngx_http_tile_layer_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_tile_loc_conf_t *tlcf = conf;
    ngx_http_core_loc_conf_t  *clcf;
    ngx_str_t                 *value;

    if (tlcf->layer.xmlname.len != 0) {
        return "is duplicate";
    }

    value = cf->args->elts;

    {
        ngx_http_conf_ctx_t *hctx = cf->ctx;

        clcf = hctx->loc_conf[ngx_http_core_module.ctx_index];
    }

    /* only prefix locations are supported (same semantics as mod_tile baseuri) */
    if (clcf->name.len == 0 || clcf->name.data[0] != '/') {
        return "must be defined inside a prefix location block";
    }

    if (clcf->alias) {
        return "alias locations are not supported";
    }

    tlcf->layer.xmlname = value[1];
    tlcf->layer.baseuri = clcf->name;

    return NGX_CONF_OK;
}

static char *
ngx_http_tile_zoom_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t  *value = cf->args->elts;
    ngx_int_t   n, *dest;

    n = ngx_atoi(value[1].data, value[1].len);
    if (n == NGX_ERROR) {
        return "is not a number";
    }

    if (n < 0 || n > NGX_TILE_MAX_ZOOM_SERVER) {
        return "the configured zoom level lies outside of the range supported "
               "by this server";
    }

    dest = (ngx_int_t *) ((char *) conf + cmd->offset);
    if (*dest != NGX_CONF_UNSET && *dest != n) {
        return "is duplicate";
    }

    *dest = n;

    return NGX_CONF_OK;
}

static char *
ngx_http_tile_hostname_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_tile_loc_conf_t *tlcf = conf;
    ngx_str_t                 *value;
    ngx_uint_t                 i;
    ngx_str_t                 *entry;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        if (tlcf->layer.hostnames == NULL) {
            tlcf->layer.hostnames = ngx_array_create(cf->pool, 4,
                sizeof(ngx_str_t));
            if (tlcf->layer.hostnames == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        entry = ngx_array_push(tlcf->layer.hostnames);
        if (entry == NULL) {
            return NGX_CONF_ERROR;
        }

        /* used verbatim as a TileJSON URL prefix (scheme included) */
        entry->data = ngx_pnalloc(cf->pool, value[i].len);
        if (entry->data == NULL) {
            return NGX_CONF_ERROR;
        }
        ngx_memcpy(entry->data, value[i].data, value[i].len);
        entry->len = value[i].len;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_tile_renderd_socket_directive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_tile_loc_conf_t *tlcf = conf;
    ngx_str_t                 *value = cf->args->elts;
    ngx_str_t                 *arg = &value[1];

    /* "unix:/path" | "host:port" | "/path" (compat: bare path = unix socket) */

    if (arg->len > 5 && ngx_strncmp(arg->data, "unix:", 5) == 0) {
        tlcf->socket_path.data = ngx_pnalloc(cf->pool, arg->len - 5 + 1);
        if (tlcf->socket_path.data == NULL) {
            return NGX_CONF_ERROR;
        }
        ngx_memcpy(tlcf->socket_path.data, arg->data + 5, arg->len - 5);
        tlcf->socket_path.data[arg->len - 5] = '\0';
        tlcf->socket_path.len = arg->len - 5;
        tlcf->renderd_port = 0;
        tlcf->renderd_socket = *arg;
        return NGX_CONF_OK;
    }

    if (arg->data[0] == '/') {
        tlcf->socket_path.data = ngx_pnalloc(cf->pool, arg->len + 1);
        if (tlcf->socket_path.data == NULL) {
            return NGX_CONF_ERROR;
        }
        ngx_memcpy(tlcf->socket_path.data, arg->data, arg->len);
        tlcf->socket_path.data[arg->len] = '\0';
        tlcf->socket_path.len = arg->len;
        tlcf->renderd_port = 0;
        tlcf->renderd_socket = *arg;
        return NGX_CONF_OK;
    }

    /* host:port form */
    {
        ngx_str_t  host;
        ngx_int_t  port;
        u_char    *colon;

        colon = ngx_strlchr(arg->data, arg->data + arg->len, ':');
        if (colon == NULL) {
            return "invalid value, expected \"unix:/path\", \"/path\" or \"host:port\"";
        }

        host.data = arg->data;
        host.len = colon - arg->data;
        port = ngx_atoi(colon + 1, arg->len - (colon - arg->data) - 1);

        if (port <= 0 || port > 65535) {
            return "invalid port";
        }

        tlcf->renderd_host.data = ngx_pnalloc(cf->pool, host.len + 1);
        if (tlcf->renderd_host.data == NULL) {
            return NGX_CONF_ERROR;
        }
        ngx_memcpy(tlcf->renderd_host.data, host.data, host.len);
        tlcf->renderd_host.data[host.len] = '\0';
        tlcf->renderd_host.len = host.len;
        tlcf->renderd_port = port;
        tlcf->renderd_socket = *arg;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_tile_factor_directive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t  *value = cf->args->elts;
    double     *dest;

    dest = (double *) ((char *) conf + cmd->offset);
    if (*dest != NGX_TILE_UNSET_DOUBLE) {
        return "is duplicate";
    }

    *dest = strtod((char *) value[1].data, NULL);
    if (*dest < 0) {
        return "is not a valid non-negative number";
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_tile_cache_low_medium_directive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_tile_loc_conf_t *tlcf = conf;
    ngx_str_t                 *value = cf->args->elts;
    ngx_int_t                  zoom, seconds;

    zoom = ngx_atoi(value[1].data, value[1].len);
    if (zoom == NGX_ERROR || zoom < 0 || zoom > NGX_TILE_MAX_ZOOM_SERVER) {
        return "invalid zoom level";
    }

    seconds = ngx_atoi(value[2].data, value[2].len);
    if (seconds == NGX_ERROR || seconds < 0) {
        return "invalid duration";
    }

    if (ngx_strcmp(cmd->name.data, "tile_cache_duration_low_zoom") == 0) {
        tlcf->cache_level_low_zoom = zoom;
        tlcf->cache_duration_low_zoom = seconds;
    } else {
        tlcf->cache_level_medium_zoom = zoom;
        tlcf->cache_duration_medium_zoom = seconds;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_tile_import_config_directive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_tile_loc_conf_t *tlcf = conf;
    ngx_str_t                 *value = cf->args->elts;
    char                      *err;

    if (tlcf->import_config.len != 0) {
        return "is duplicate";
    }

    tlcf->import_config = value[1];

    err = ngx_http_tile_import_config(cf, tlcf);
    if (err != NULL) {
        return err;
    }

    return NGX_CONF_OK;
}

/* ------------------------------------------------------------------ */
/* renderd.conf import (port of process_map_sections)                  */
/* ------------------------------------------------------------------ */

static char *
tile_copy_pool_string(ngx_pool_t *pool, const char *src, size_t maxlen,
    ngx_str_t *dest)
{
    size_t len;

    len = strnlen(src, maxlen);
    dest->data = ngx_pnalloc(pool, len + 1);
    if (dest->data == NULL) {
        return (char *) NGX_CONF_ERROR;
    }

    ngx_memcpy(dest->data, src, len);
    dest->data[len] = '\0';
    dest->len = len;

    return NULL;
}

char *
ngx_http_tile_import_config(ngx_conf_t *cf, ngx_http_tile_loc_conf_t *tlcf)
{
    dictionary               *ini;
    ngx_http_tile_layer_t    *layers;
    ngx_uint_t                n_sections, n, count = 0;
    const char               *section, *str;
    char                      key[512];
    u_char                    buf[512];
    char                     *err;

    ini = iniparser_load((const char *) tlcf->import_config.data);
    if (ini == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "tile_import_config: failed to load config file \"%V\"",
                           &tlcf->import_config);
        return NGX_CONF_ERROR;
    }

    n_sections = (ngx_uint_t) iniparser_getnsec(ini);

    layers = ngx_pcalloc(cf->pool,
                         sizeof(ngx_http_tile_layer_t) * NGX_TILE_XMLCONFIGS_MAX);
    if (layers == NULL) {
        iniparser_freedict(ini);
        return NGX_CONF_ERROR;
    }

    for (n = 0; n < n_sections; n++) {
        ngx_http_tile_layer_t  *layer;
        int                     zoom;
        int                     type_parts = 0;
        char                   *type_copy, *sp, *tok;
        const char             *type_str;

        section = iniparser_getsecname(ini, (int) n);

        /* sections not named "renderd*"/"mapnik" are map sections */
        if (ngx_strncmp((u_char *) section, (u_char *) "renderd", 7) == 0
            || ngx_strcmp((u_char *) section, (u_char *) "mapnik") == 0)
        {
            continue;
        }

        if (count >= NGX_TILE_XMLCONFIGS_MAX) {
            iniparser_freedict(ini);
            return "tile_import_config: too many map sections";
        }

        layer = &layers[count++];

        ngx_memzero(layer, sizeof(*layer));

        /* xmlname (section name, XMLCONFIG_MAX limit) */
        if (tile_copy_pool_string(cf->pool, section, XMLCONFIG_MAX,
                                  &layer->xmlname) != NULL)
        {
            return NGX_CONF_ERROR;
        }

#define TILE_INI_KEY(name) \
    do {                                                                \
        int kl = snprintf(key, sizeof(key), "%s:%s", section, name);     \
        if (kl < 0 || (size_t) kl >= sizeof(key)) { key[0] = '\0'; }     \
    } while (0)

#define TILE_COPY_FIELD(name, dest, maxlen)                             \
    do {                                                                \
        TILE_INI_KEY(name);                                             \
        str = iniparser_getstring(ini, key, "");                        \
        err = tile_copy_pool_string(cf->pool, str, maxlen, &dest);       \
        if (err != NULL) {                                              \
            iniparser_freedict(ini);                                    \
            return NGX_CONF_ERROR;                                      \
        }                                                               \
    } while (0)

#define TILE_INT_FIELD(name, dest, notfound)                            \
    do {                                                                \
        TILE_INI_KEY(name);                                             \
        dest = iniparser_getint(ini, key, notfound);                    \
    } while (0)

        TILE_INT_FIELD("aspectx", layer->aspect_x, 1);
        TILE_INT_FIELD("aspecty", layer->aspect_y, 1);

        TILE_COPY_FIELD("attribution", layer->attribution, PATH_MAX);
        TILE_COPY_FIELD("cors", layer->cors, PATH_MAX);
        TILE_COPY_FIELD("description", layer->description, PATH_MAX);
        TILE_COPY_FIELD("tiledir", layer->store_uri, PATH_MAX);

        TILE_INT_FIELD("maxzoom", zoom, MAX_ZOOM);
        if (zoom < 0 || zoom > MAX_ZOOM) {
            iniparser_freedict(ini);
            return "tile_import_config: max zoom outside of 0..20";
        }
        layer->maxzoom = zoom;

        TILE_INT_FIELD("minzoom", zoom, 0);
        if (zoom < 0 || zoom > layer->maxzoom) {
            iniparser_freedict(ini);
            return "tile_import_config: min zoom invalid";
        }
        layer->minzoom = zoom;

        /* uri (baseuri), trailing slash enforced */
        {
            size_t ulen;

            TILE_INI_KEY("uri");
            str = iniparser_getstring(ini, key, "");

            ulen = strnlen(str, PATH_MAX);
            layer->baseuri.data = ngx_pnalloc(cf->pool, ulen + 2);
            if (layer->baseuri.data == NULL) {
                iniparser_freedict(ini);
                return NGX_CONF_ERROR;
            }
            ngx_memcpy(layer->baseuri.data, str, ulen);
            if (ulen == 0 || layer->baseuri.data[ulen - 1] != '/') {
                layer->baseuri.data[ulen++] = '/';
            }
            layer->baseuri.data[ulen] = '\0';
            layer->baseuri.len = ulen;
        }

        /* type: "<extension> <mime_type> [output_format]" */
        TILE_INI_KEY("type");
        type_str = iniparser_getstring(ini, key, "png image/png png256");

        type_copy = strndup(type_str, NGX_TILE_INILINE_MAX);
        if (type_copy == NULL) {
            iniparser_freedict(ini);
            return NGX_CONF_ERROR;
        }

        sp = NULL;
        for (tok = strtok_r(type_copy, " ", &sp); tok != NULL;
             tok = strtok_r(NULL, " ", &sp))
        {
            switch (type_parts) {
            case 0:
                err = tile_copy_pool_string(cf->pool, tok, 64,
                                            &layer->file_extension);
                break;
            case 1:
                err = tile_copy_pool_string(cf->pool, tok, 64,
                                            &layer->mime_type);
                break;
            default:
                err = (char *) "tile_import_config: type has too many parts";
                break;
            }

            if (err != NULL) {
                free(type_copy);
                iniparser_freedict(ini);
                if (err == NGX_CONF_ERROR) {
                    return NGX_CONF_ERROR;
                }
                return err;
            }

            type_parts++;
        }

        free(type_copy);

        if (type_parts < 2) {
            iniparser_freedict(ini);
            return "tile_import_config: type must have at least 2 parts";
        }

        /* parameterize_style enables /<options>/z/x/y.ext URLs */
        TILE_INI_KEY("parameterize_style");
        str = iniparser_getstring(ini, key, "");
        if (str[0] != '\0') {
            layer->enable_options = 1;
            (void) ngx_snprintf(buf, sizeof(buf), "parameterize_style \"%s\" is "
                                "accepted but style parameterization is not "
                                "supported by this build; URLs with options "
                                "are parsed but options are ignored", str);
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0, (char *) buf);
        }
    }

    iniparser_freedict(ini);

    if (count == 0) {
        return "tile_import_config: no map sections found in file";
    }

    /* shadowing validation: an earlier uri must not be a prefix of a later one */
    for (n = 0; n + 1 < count; n++) {
        ngx_uint_t m;

        for (m = n + 1; m < count; m++) {
            if (layers[n].baseuri.len != 0
                && ngx_strncmp(layers[n].baseuri.data, layers[m].baseuri.data,
                               layers[n].baseuri.len) == 0)
            {
                return "tile_import_config: an earlier map uri is a prefix of a "
                       "later map uri and would shadow it";
            }
        }
    }

    tlcf->imported_layers = ngx_array_create(cf->pool, count,
                                             sizeof(ngx_http_tile_layer_t));
    if (tlcf->imported_layers == NULL) {
        return NGX_CONF_ERROR;
    }

    {
        ngx_uint_t i;

        for (i = 0; i < count; i++) {
            ngx_http_tile_layer_t *entry = ngx_array_push(tlcf->imported_layers);
            if (entry == NULL) {
                return NGX_CONF_ERROR;
            }
            *entry = layers[i];
        }
    }

    return NGX_CONF_OK;
}
