#define _POSIX_C_SOURCE 200809L

#include "status_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Upper bound for the formatted blob. The RX status table has grown to ~45
 * counters; at the u64 worst case (20-digit values) that exceeds 1 KiB, and
 * status_write returns -1 (writes NOTHING -- never truncates) if it overflows,
 * which would silently drop the whole snapshot. 2 KiB keeps a comfortable
 * margin and still bounds the stack buffer in status_write. */
#define STATUS_BUF_CAP 2048

int status_format(char *buf, size_t cap, const StatusKV *kvs, size_t n) {
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        /* snprintf returns the length it WOULD have written; use that to
         * detect overflow without writing past `cap`. */
        int w = snprintf(buf + used, cap - used, "%s=%llu\n",
                         kvs[i].key, kvs[i].val);
        if (w < 0) return -1;
        if ((size_t)w >= cap - used) return -1;   /* would truncate */
        used += (size_t)w;
    }
    return (int)used;
}

int status_write(const char *path, const StatusKV *kvs, size_t n) {
    char buf[STATUS_BUF_CAP];
    char tmp[256];
    int  fd, w;
    ssize_t wr;

    w = status_format(buf, sizeof(buf), kvs, n);
    if (w < 0) { errno = ENOBUFS; return -1; }   /* counters exceed STATUS_BUF_CAP */

    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
        errno = ENAMETOOLONG;                    /* path genuinely too long */
        return -1;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;

    wr = write(fd, buf, (size_t)w);
    if (wr != (ssize_t)w) {
        int e = (wr < 0) ? errno : EIO;
        close(fd);
        unlink(tmp);
        errno = e;
        return -1;
    }
    if (close(fd) < 0) { unlink(tmp); return -1; }

    /* rename within the same tmpfs dir is atomic -- the reader sees either
     * the old file or the fully-written new one, never a partial. */
    if (rename(tmp, path) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    return 0;
}
