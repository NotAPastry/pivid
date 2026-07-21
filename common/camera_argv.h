#ifndef COMMON_CAMERA_ARGV_H
#define COMMON_CAMERA_ARGV_H

#include "settings.h"

/* Fixed upper bound on emitted argv slots (largest recipe = droppable, argc=29,
 * needs 30 slots incl. argv[0] and the NULL terminator). Generous headroom so
 * the builder never heaps. */
#define CAMERA_ARGV_MAX       40
/* Backing bytes for one numeric flag value ("-2147483648" + NUL fits in 12). */
#define CAMERA_ARGV_VALBYTES  16

/* A fully-built rpicam-vid argument vector. argv[] is NULL-terminated and ready
 * for execvp(); entries point either at string literals (constant flags) or into
 * store[] (formatted numeric values). Self-contained: copy/pass by value safely
 * ONLY before any pointer is dereferenced across an address-space boundary --
 * after fork() the child uses its own copy, which is fine. */
typedef struct {
    char *argv[CAMERA_ARGV_MAX];
    char  store[CAMERA_ARGV_MAX][CAMERA_ARGV_VALBYTES];
    int   argc;
} CameraArgv;

/* Build the rpicam-vid argv for `s` into `out`, with `bin` as argv[0]:
 * constant flags + per-recipe flag set + the gdr_idr intra/intra-refresh
 * split. Returns 0 on success, -1 on an unknown recipe or
 * argv overflow. Pure -- no spawn, no env, no I/O. */
int camera_build_argv(const Settings *s, const char *bin, CameraArgv *out);

#endif /* COMMON_CAMERA_ARGV_H */
