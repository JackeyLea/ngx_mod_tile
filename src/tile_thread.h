/*
 * ngx_mod_tile - thread-pool offload for blocking storage backends.
 *
 * Network-backed stores (memcached, rados, composite, ro_http_proxy) block;
 * running them in the event loop would stall the worker. With
 * `tile_thread_pool <name>;` tile_stat / tile_read run as ngx_thread_pool
 * tasks and the request resumes through the event handler.
 *
 * Each task owns a private pool that survives request aborts (the completion
 * event checks a cleanup-set flag before touching anything), and each pool
 * thread uses its own storage_backend instance (pthread TLS) so blocking
 * backends with per-connection state stay safe under concurrency.
 */

#ifndef NGX_TILE_THREAD_H
#define NGX_TILE_THREAD_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_mod_tile.h"

/* thread_stage values on the request context */
#define NGX_TILE_THREAD_NONE 0
#define NGX_TILE_THREAD_STAT 1
#define NGX_TILE_THREAD_READ 2

ngx_int_t ngx_http_tile_thread_stat(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
ngx_int_t ngx_http_tile_thread_read(ngx_http_request_t *r,
    ngx_http_tile_ctx_t *ctx);
ngx_int_t ngx_http_tile_thread_resume(ngx_http_request_t *r,
    ngx_uint_t stage);

#endif /* NGX_TILE_THREAD_H */
