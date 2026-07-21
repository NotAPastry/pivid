/* O_CLOEXEC and related flags live behind POSIX.1-2008 in glibc fcntl.h. */
#define _POSIX_C_SOURCE 200809L

#include "tx.h"

#include "aqm.h"
#include "ring_user.h"
#include "radio_ioctl.h"
#include "tx_sample.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEV_PATH "/dev/radio0"

int tx_open(TX_Dev *dev, uint32_t initial_sync_word) {
    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;

    if (pthread_mutex_init(&dev->producer_lock, NULL) != 0) {
        perror("pthread_mutex_init producer_lock");
        return -1;
    }

    dev->fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);
    if (dev->fd < 0) {
        perror("open " DEV_PATH);
        pthread_mutex_destroy(&dev->producer_lock);
        return -1;
    }

    /* mmap dispatch: offset = RADIO_MMAP_TX_OFFSET (=0) selects the TX ring. */
    dev->tx_ring = mmap(NULL, RADIO_RING_BYTES, PROT_READ | PROT_WRITE,
                        MAP_SHARED, dev->fd, RADIO_MMAP_TX_OFFSET);
    if (dev->tx_ring == MAP_FAILED) {
        perror("mmap TX ring");
        dev->tx_ring = NULL;
        close(dev->fd);
        dev->fd = -1;
        pthread_mutex_destroy(&dev->producer_lock);
        return -1;
    }

    if (tx_set_sync_word(dev, initial_sync_word) < 0) {
        perror("ioctl SET_SYNC_WORD");
        tx_close(dev);
        return -1;
    }
    if (tx_set_mode(dev, RADIO_MODE_TX) < 0) {
        perror("ioctl SET_MODE");
        tx_close(dev);
        return -1;
    }
    return 0;
}

/* Retry the ioctl across EINTR. Other failures surface to the caller. */
static int ioctl_eintr(int fd, unsigned long req, void *arg) {
    int rc;
    do {
        rc = (arg != NULL) ? ioctl(fd, req, arg) : ioctl(fd, req);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static int push_locked(TX_Dev *dev, const uint8_t *bytes, size_t len,
                       int count_full) {
    int rc = 0;
    /* Held across ring_user_push + the doorbell ioctl + counter updates so the
     * MSP poller thread can call tx_push concurrently with the encoder thread
     * without tearing the SPSC ring's wr_idx. Contention is trivial (10 Hz
     * telemetry vs ~1000 Hz video). */
    pthread_mutex_lock(&dev->producer_lock);
    if (ring_user_push(dev->tx_ring, bytes, len) < 0) {
        if (count_full) dev->dropped_full++;
        rc = -1;
        goto out;
    }
    /* AQM instrumentation (dormant unless PIVID_TX_SAMPLE): snapshot the
     * just-published wr_idx -- the record's end position -- under this lock,
     * so it matches the wr_idx the sampler thread will chase. No-op on NULL. */
    tx_sample_record(dev->sample, *ring_wr_ptr(dev->tx_ring));
    /* AQM per-frame peak occupancy (dormant unless AQM): same lock, same
     * just-published wr; reads rd here too. No-op on NULL/OFF. */
    aqm_on_push(dev->aqm, *ring_wr_ptr(dev->tx_ring), *ring_rd_ptr(dev->tx_ring));
    /* The packet is now in the ring. Count it as sent regardless of the
     * doorbell outcome -- if the kernel ioctl fails (vanishingly rare;
     * one queue_work call), the next successful push doorbells again and
     * the worker drains both records. dropped_ioctl is a separate diagnostic
     * counter for doorbell failures, NOT a drop counter. */
    dev->packets_sent++;
    dev->bytes_sent += len;
    if (ioctl_eintr(dev->fd, RADIO_IOCTL_TX_ENQUEUE, NULL) < 0) {
        dev->dropped_ioctl++;
        rc = -2;
    }
out:
    pthread_mutex_unlock(&dev->producer_lock);
    return rc;
}

int tx_push(TX_Dev *dev, const uint8_t *bytes, size_t len) {
    return push_locked(dev, bytes, len, 1);
}

int tx_push_try(TX_Dev *dev, const uint8_t *bytes, size_t len) {
    return push_locked(dev, bytes, len, 0);
}

int tx_set_mode(TX_Dev *dev, int mode) {
    __s32 m = (__s32)mode;
    return (ioctl_eintr(dev->fd, RADIO_IOCTL_SET_MODE, &m) < 0) ? -1 : 0;
}

int tx_set_sync_word(TX_Dev *dev, uint32_t sw) {
    __u32 v = (__u32)sw;
    return (ioctl_eintr(dev->fd, RADIO_IOCTL_SET_SYNC_WORD, &v) < 0) ? -1 : 0;
}

int tx_get_stats(TX_Dev *dev, struct radio_stats *out) {
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

void tx_close(TX_Dev *dev) {
    if (dev->tx_ring) {
        munmap(dev->tx_ring, RADIO_RING_BYTES);
        dev->tx_ring = NULL;
    }
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
    /* pthread_mutex_destroy is NOT async-signal-safe; tx_close runs only from
     * main() during shutdown, never from a signal handler. */
    pthread_mutex_destroy(&dev->producer_lock);
}
