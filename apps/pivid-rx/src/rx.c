/* O_CLOEXEC and related flags live behind POSIX.1-2008 in glibc fcntl.h. */
#define _POSIX_C_SOURCE 200809L

#include "rx.h"

#include "ring_user.h"
#include "radio_ioctl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEV_PATH "/dev/radio0"

int rx_open(RX_Dev *dev, uint32_t initial_sync_word) {
    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;

    dev->fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);
    if (dev->fd < 0) {
        perror("open " DEV_PATH);
        return -1;
    }

    /* PROT_READ | PROT_WRITE: the consumer (us) advances rd_idx via an
     * atomic release store in ring_user_pop. The kernel patch in this
     * commit removes the prior PROT_READ-only enforcement on the RX ring;
     * data bytes remain read-only-from-userspace by convention. */
    dev->rx_ring = mmap(NULL, RADIO_RING_BYTES, PROT_READ | PROT_WRITE,
                        MAP_SHARED, dev->fd, RADIO_MMAP_RX_OFFSET);
    if (dev->rx_ring == MAP_FAILED) {
        perror("mmap RX ring");
        dev->rx_ring = NULL;
        close(dev->fd);
        dev->fd = -1;
        return -1;
    }

    if (rx_set_sync_word(dev, initial_sync_word) < 0) {
        perror("ioctl SET_SYNC_WORD");
        rx_close(dev);
        return -1;
    }
    if (rx_set_mode(dev, RADIO_MODE_RX) < 0) {
        perror("ioctl SET_MODE");
        rx_close(dev);
        return -1;
    }
    return 0;
}

int rx_pop(RX_Dev *dev, uint8_t *out, size_t out_max) {
    int n = ring_user_pop(dev->rx_ring, out, out_max);
    if (n > 0) {
        dev->packets_received++;
        dev->bytes_received += (uint64_t)n;
    } else if (n < 0) {
        dev->dropped_torn++;
    }
    return n;
}

/* Retry the ioctl across EINTR. Other failures surface to the caller.
 * Mirrors the helper in apps/pivid-tx/src/tx.c -- intentionally duplicated
 * rather than lifted to a shared header because the call surface is tiny. */
static int ioctl_eintr(int fd, unsigned long req, void *arg) {
    int rc;
    do { rc = ioctl(fd, req, arg); } while (rc < 0 && errno == EINTR);
    return rc;
}

int rx_set_mode(RX_Dev *dev, int mode) {
    __s32 m = (__s32)mode;
    return (ioctl_eintr(dev->fd, RADIO_IOCTL_SET_MODE, &m) < 0) ? -1 : 0;
}

int rx_set_sync_word(RX_Dev *dev, uint32_t sw) {
    __u32 v = (__u32)sw;
    return (ioctl_eintr(dev->fd, RADIO_IOCTL_SET_SYNC_WORD, &v) < 0) ? -1 : 0;
}

int rx_get_rssi(RX_Dev *dev, int8_t *out_rssi) {
    __s8 r = 0;
    if (ioctl_eintr(dev->fd, RADIO_IOCTL_GET_RSSI, &r) < 0) return -1;
    dev->rssi_dbm = (int8_t)r;
    if (out_rssi) *out_rssi = (int8_t)r;
    return 0;
}

int rx_get_cal_diff(RX_Dev *dev, int32_t *out_db) {
    __s32 d = 0;
    if (ioctl_eintr(dev->fd, RADIO_IOCTL_GET_CAL_DIFF, &d) < 0) return -1;
    if (out_db) *out_db = (int32_t)d;
    return 0;
}

int rx_get_stats(RX_Dev *dev, struct radio_stats *out) {
    memset(out, 0, sizeof(*out));
    if (ioctl_eintr(dev->fd, RADIO_IOCTL_GET_STATS, out) < 0) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    if (out->version != RADIO_STATS_VERSION) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

void rx_close(RX_Dev *dev) {
    if (dev->rx_ring) {
        munmap(dev->rx_ring, RADIO_RING_BYTES);
        dev->rx_ring = NULL;
    }
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}
