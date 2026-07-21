#ifndef RX_RX_H
#define RX_RX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int   fd;            /* /dev/radio0 fd, -1 when closed                  */
    void *rx_ring;       /* mmap'd RADIO_RING_BYTES (RW so we can advance rd_idx) */

    /* Stats (main thread is sole writer). */
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t dropped_torn;    /* ring_user_pop returned -1 -- never expected */
    int8_t   rssi_dbm;        /* last cached RSSI from RADIO_IOCTL_GET_RSSI  */
} RX_Dev;

/* Open /dev/radio0, mmap RX ring at offset RADIO_MMAP_RX_OFFSET, set mode=RX,
 * set sync word. Returns 0 on success, -1 on any error (errno preserved). */
int rx_open(RX_Dev *dev, uint32_t initial_sync_word);

/* Pop one record. Returns:
 *   >0 : record length in bytes (1..511); payload copied into out
 *    0 : ring empty
 *   -1 : ring corrupt OR record larger than out_max (bumps dropped_torn)
 * `out_max` must be >= 511 (the FLRC payload ceiling); a smaller buffer makes
 * ring_user_pop report every oversized record as -1/torn. */
int rx_pop(RX_Dev *dev, uint8_t *out, size_t out_max);

/* Thin ioctl wrappers. Return 0 on success, -1 on failure (errno preserved). */
int rx_set_mode(RX_Dev *dev, int mode);            /* RADIO_MODE_RX / BIND_LISTEN */
int rx_set_sync_word(RX_Dev *dev, uint32_t sw);
int rx_get_rssi(RX_Dev *dev, int8_t *out_rssi);    /* refreshes dev->rssi_dbm too */
int rx_get_cal_diff(RX_Dev *dev, int32_t *out_db); /* measure-only cal quality (dB) */

/* Snapshot driver counters via RADIO_IOCTL_GET_STATS. Returns 0 and fills
 * `out` on success (version validated); -1 on ioctl failure or version
 * mismatch. On -1 `out` is zeroed so the caller renders an all-zero kernel
 * section without branching. */
struct radio_stats;   /* from radio_ioctl.h */
int rx_get_stats(RX_Dev *dev, struct radio_stats *out);

/* Idempotent. Safe to call twice. */
void rx_close(RX_Dev *dev);

#endif
