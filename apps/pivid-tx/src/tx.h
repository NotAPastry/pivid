#ifndef TX_TX_H
#define TX_TX_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TX_Dev {
    int   fd;            /* /dev/radio0 fd, -1 when closed                  */
    void *tx_ring;       /* mmap'd RADIO_RING_BYTES (RW), NULL when closed  */

    /* Stats (main thread is sole writer; GPIO thread reads dropped/sent for logs). */
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t dropped_full;
    uint64_t dropped_ioctl;

    /* Producer-side lock. tx_push() holds this around ring_user_push + the
     * doorbell ioctl + counter updates so the MSP poller thread can call
     * tx_push concurrently with the encoder thread without tearing the SPSC
     * ring's wr_idx. */
    pthread_mutex_t producer_lock;

    /* AQM baseline instrumentation (tx_sample.*). NULL unless main wired a
     * sampler; the tx_push hook no-ops on NULL/dormant, so the default build
     * is unaffected. */
    struct TxSample *sample;

    /* Feeder-level AQM (aqm.*). NULL unless the encoder wired one; the tx_push
     * hook no-ops on NULL/OFF, so the default build is unaffected. */
    struct Aqm *aqm;
} TX_Dev;

/* Open /dev/radio0, mmap TX ring at offset 0, set mode=TX, set sync word.
 * Returns 0 on success, -1 on any error (errno preserved). */
int tx_open(TX_Dev *dev, uint32_t initial_sync_word);

/* Push one record + ring doorbell. Returns:
 *   0  : enqueued, ioctl OK -> packets_sent++, bytes_sent += len
 *  -1  : ring full          -> dropped_full++
 *  -2  : ioctl failed       -> dropped_ioctl++ (treat as transient; keep going)
 * `bytes` is the protocol packet (flags + [len] + payload) from protocol_build;
 * `len` must be <= 511 (the FLRC 511 B on-air ceiling / 2-byte ring length prefix). */
int tx_push(TX_Dev *dev, const uint8_t *bytes, size_t len);

/* Like tx_push, but a full ring does NOT count dropped_full: -1 means "try
 * again". For producers that retry a microburst-full ring (the video path)
 * instead of silently losing the packet -- a full ring drains a slot every
 * ~2.5 ms, so a brief bounded wait beats a guaranteed slice loss (bottom-half
 * corruption RCA, 2026-07-05). Use tx_push for fire-and-forget producers
 * (MSP telemetry) where dropping fast is correct. */
int tx_push_try(TX_Dev *dev, const uint8_t *bytes, size_t len);

/* Video-path retry budget: TX_PUSH_TRIES uncounted attempts spaced
 * TX_PUSH_RETRY_WAIT_US apart (~50 ms worst case), then one final counted
 * tx_push. Referenced by tests/tx/test_encoder.c (expects TRIES+1 attempts). */
#define TX_PUSH_TRIES          25
#define TX_PUSH_RETRY_WAIT_US  2000

/* Thin ioctl wrappers. Return 0 on success, -1 on failure (errno preserved). */
int tx_set_mode(TX_Dev *dev, int mode);          /* RADIO_MODE_TX / BIND_TRANSMIT */
int tx_set_sync_word(TX_Dev *dev, uint32_t sw);

/* Snapshot driver counters via RADIO_IOCTL_GET_STATS. Returns 0 and fills
 * `out` on success (version validated); -1 on ioctl failure or version
 * mismatch (errno preserved on ioctl failure). On -1 `out` is zeroed so the
 * caller can render an all-zero kernel section without branching. */
struct radio_stats;   /* from radio_ioctl.h */
int tx_get_stats(TX_Dev *dev, struct radio_stats *out);

/* Idempotent. Call tx_close only from main() during shutdown (after
 * signalling the encoder thread via wake_fd), not from a signal handler --
 * pthread_mutex_destroy is not async-signal-safe. The existing shutdown path
 * already does this. */
void tx_close(TX_Dev *dev);

#endif
