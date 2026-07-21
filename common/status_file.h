#ifndef COMMON_STATUS_FILE_H
#define COMMON_STATUS_FILE_H

#include <stddef.h>

/* One counter, by display name. Value is always rendered as an unsigned
 * decimal. Keys form the soft contract consumed by the future MSP/OSD
 * overlay -- keep them stable. */
typedef struct {
    const char         *key;
    unsigned long long  val;
} StatusKV;

/* Format `n` key=value lines ("<key>=<val>\n" each) into `buf`. Returns the
 * number of bytes written (excluding the terminating NUL), or -1 if `buf`
 * (capacity `cap`) is too small for the full output (never truncates). */
int status_format(char *buf, size_t cap, const StatusKV *kvs, size_t n);

/* Atomically (re)write `path` with the formatted counters: format into a
 * stack buffer, write to "<path>.tmp", then rename() over `path` so a reader
 * never observes a partial write. Returns 0 on success, -1 on any error
 * (errno preserved). Intended for a tmpfs path (e.g. /run/pivid/tx.status)
 * so the rewrite costs no flash wear. */
int status_write(const char *path, const StatusKV *kvs, size_t n);

#endif
