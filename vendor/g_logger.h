/*
 * Drop-in replacement for mod_tile's glib-based logging shim.
 *
 * The vendored mod_tile sources (store backends, sys_utils, renderd.conf
 * parser) only ever call g_logger() with glib's G_LOG_LEVEL_* constants.
 * This header re-declares the same API and constants without requiring a
 * glib development dependency. The implementation lives in g_logger_ngx.c
 * and forwards to nginx's ngx_log_error_core().
 */

#ifndef TILE_G_LOGGER_H
#define TILE_G_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Same values as glib's GLogLevelFlags */
#define G_LOG_LEVEL_ERROR    (1 << 2)
#define G_LOG_LEVEL_CRITICAL (1 << 3)
#define G_LOG_LEVEL_WARNING  (1 << 4)
#define G_LOG_LEVEL_MESSAGE  (1 << 5)
#define G_LOG_LEVEL_INFO     (1 << 6)
#define G_LOG_LEVEL_DEBUG    (1 << 7)

/* Kept for API parity with upstream g_logger.h (always "foreground" here) */
extern int foreground;

void g_logger(int log_level, const char *format, ...);
const char *g_logger_level_name(int log_level);

#ifdef __cplusplus
}
#endif

#endif /* TILE_G_LOGGER_H */
