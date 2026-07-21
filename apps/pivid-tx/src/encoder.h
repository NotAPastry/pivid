#ifndef TX_ENCODER_H
#define TX_ENCODER_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "aqm.h"
#include "fec_tx.h"
#include "sps_reinject.h"
#include "tx.h"

/* Maximum NAL size the parser will hold internally. Matches
 * REASM_BUF_BYTES on the RX side (apps/pivid-rx/src/reasm.h) -- a NAL
 * that exceeds either limit can't survive the radio link anyway. */
#define ENCODER_NAL_BUF_BYTES 65536

/* Typed encoder configuration, filled by main() from Settings -- the single
 * translation point from config to the encoder. Keeps the encoder free of
 * getenv() for production knobs. */
typedef struct {
    int fec_m_anchor, fec_m_ref, fec_k;
    int sps_reinject_ms;
    AqmConfig aqm;       /* mode + derived recipe (tuning still hidden env) */
} EncoderConfig;

typedef void (*Encoder_NalEmit)(const uint8_t *nal, size_t len, void *user);

typedef struct {
    uint8_t nal_buf[ENCODER_NAL_BUF_BYTES];
    size_t  nal_len;
    uint8_t state;       /* 0 = IDLE, 1 = MATCHING, 2 = IN_NAL */
    uint8_t zeros;       /* count of consecutive 0x00 at tail of nal_buf
                          * during MATCHING; 0 elsewhere */
    uint64_t nals_emitted;
    uint64_t nals_oversize;  /* incremented when a NAL would exceed
                              * ENCODER_NAL_BUF_BYTES; truncated then
                              * emitted */

    /* IMP-1 residency probe (measurement only; see common/resid_hist.h). The
     * caller stamps feed_now_ns with the arrival time of the bytes it is about
     * to hand to _feed(); the parser copies it into cur_arrival_ns as it
     * appends content, and snapshots it into emit_arrival_ns at each emit so
     * the emit callback can compute now-arrival. All zero for callers (tests)
     * that never set feed_now_ns, which disables the measurement cleanly. */
    uint64_t feed_now_ns;    /* arrival time of the current _feed() buffer     */
    uint64_t cur_arrival_ns; /* arrival of the in-progress NAL's last content  */
    uint64_t emit_arrival_ns;/* arrival snapshot of the just-emitted NAL       */
} Encoder_NalParser;

void encoder_nal_parser_init  (Encoder_NalParser *p);
void encoder_nal_parser_feed  (Encoder_NalParser *p,
                               const uint8_t *buf, size_t n,
                               Encoder_NalEmit emit, void *user);
void encoder_nal_parser_finish(Encoder_NalParser *p,
                               Encoder_NalEmit emit, void *user);

typedef struct {
    TX_Dev *dev;                         /* owned by main */
    int     cam_fd;                      /* camera pipe read-end (was stdin) */
    int     wake_fd;                     /* eventfd, set by GPIO thread or signal handler */

    uint8_t seq_counter;                 /* 0..15, monotonic mod 16; never reset */

    /* Cross-thread shared.
     * last_data_ns: GPIO thread reads to drive the TX LED. RELAXED store
     * is fine (no data dependency); reads also RELAXED with a < 1s
     * staleness tolerance. */
    _Atomic uint64_t last_data_ns;

    /* Heartbeat tick state -- main resets last_stats_ms after logging. */
    uint64_t last_stats_ms;

    /* Stateful Annex-B parser: camera pipe reads feed into this, and the
     * parser emits one complete NAL at a time to emit_nal_to_chunker, which
     * routes it through the FEC stage. */
    Encoder_NalParser parser;

    FecTx fec;            /* TX FEC-encode stage (m=0 => passthrough) */
    Aqm aqm;              /* feeder-level frame-drop AQM (OFF unless AQM) */

    /* Periodic SPS/PPS re-injection for intra-refresh (GDR) streams, where the
     * encoder emits params only at the single startup IDR. Off (interval 0) by
     * default; enabled via SPS_REINJECT_MS for the GDR recipe. */
    SpsReinject sps_reinject;

    /* DEBUG ONLY: PIVID_TX_DROP_PCT deterministic pre-transmit loss injection.
     * Drops built packets before tx_push -> RX sees genuine on-air gaps WITHOUT
     * perturbing RX timing (unlike an RX-side drop, which amplifies the kernel
     * SPI misframe). Off (0) in production. */
    uint32_t tx_drop_pct;
    uint64_t tx_drop_state;
    uint64_t tx_injected_drops;

    /* Ring-full retry accounting (fec_push_to_ring bounded retry): total
     * retry attempts across all packets. A rising rate flags sustained ring
     * pressure before packets are actually lost (dropped_full). */
    uint64_t tx_push_retries;
} Encoder;

/* Initialize encoder state (zero counters, store fds). Does NOT take
 * ownership of `dev` or `wake_fd`. */
void encoder_init(Encoder *e, TX_Dev *dev, int cam_fd, int wake_fd,
                  const EncoderConfig *cfg);

/* Re-point the encoder's input fd (used after a camera respawn hands us a new
 * pipe read-end). Takes effect on the next encoder_run() poll. */
void encoder_set_input_fd(Encoder *e, int cam_fd);

/* Applied FEC parameters (post-clamp), for tests/status. */
int encoder_get_fec_anchor(const Encoder *e);
int encoder_get_fec_k(const Encoder *e);

/* Run the encoder loop until the camera pipe closes (EOF), the wake_fd is signaled, or the
 * 5 s stats heartbeat is due. Returns:
 *    3 if heartbeat due (caller logs + resets last_stats_ms, then resumes)
 *    2 if wake_fd was set (bind requested or shutdown)
 *    1 if camera pipe EOF (rpicam-vid died)             -> main: reap + respawn
 *                                                          (only g_shutdown exits)
 *   -1 on unrecoverable error (errno preserved). */
int encoder_run(Encoder *e);

/* Live FEC reconfigure (SIGHUP reload path): flush any open block, then
 * re-init the FEC encoder with new parity/k. Out-of-range values are ignored
 * (the current config is kept). */
void encoder_set_fec(Encoder *e, int m_anchor, int m_ref, int k);

#endif
