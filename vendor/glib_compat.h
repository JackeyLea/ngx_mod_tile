/* Compatibility shim for glib.h in vendored mod_tile sources.
 * The backends only include glib for standard types/macros; no glib runtime
 * symbols are used, so we can safely map to standard headers. */
#ifndef TILE_GLIB_COMPAT_H
#define TILE_GLIB_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif /* TILE_GLIB_COMPAT_H */
