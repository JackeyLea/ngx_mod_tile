/*
 * Compat header replacing mod_tile's autoconf-generated config.h for the
 * nginx module build. Only feature switches actually consumed by the
 * vendored sources are defined here.
 */

#ifndef TILE_COMPAT_H
#define TILE_COMPAT_H

/* Pull in the feature macros emitted by the nginx configure/addon scripts.
 * The vendored backends do not include ngx_config.h themselves. */
#include <ngx_auto_config.h>

/* sys_utils.c: use getloadavg() instead of parsing /proc/loadavg */
#define HAVE_GETLOADAVG 1

/* package identity (unused by the vendored serving path, kept for parity) */
#define PACKAGE "ngx_mod_tile"
#define VERSION "0.1.0"

#endif /* TILE_COMPAT_H */
