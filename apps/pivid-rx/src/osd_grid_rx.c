/* apps/pivid-rx/src/osd_grid_rx.c */
#include "osd_grid_rx.h"
#include <string.h>

/* Seqlock helpers — mirrors the msp_telemetry.c fence-based seqlock pattern exactly.
 * relaxed fetch_add keeps the increment outside the fence window; the explicit
 * release fence orders the guarded store (r->grid = ...) between the two increments. */
static void wr_begin(RxGrid *r) {
    atomic_fetch_add_explicit(&r->seq, 1, memory_order_relaxed);  /* -> odd */
    atomic_thread_fence(memory_order_release);
}
static void wr_end(RxGrid *r) {
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&r->seq, 1, memory_order_relaxed);  /* -> even */
}

void rx_grid_init(RxGrid *r, uint8_t rssi_row, uint8_t rssi_col,
                  uint8_t lq_row, uint8_t lq_col) {
    memset(r, 0, sizeof *r);
    osd_grid_clear(&r->grid);
    osd_grid_reasm_init(&r->reasm);
    atomic_store(&r->vid_rssi_dbm, 0);
    r->vid_lq_pct = OSD_VID_LQ_NA;
    r->rssi_row = rssi_row; r->rssi_col = rssi_col;
    r->lq_row = lq_row; r->lq_col = lq_col;
}

void rx_grid_set_cells(RxGrid *r, uint8_t rssi_row, uint8_t rssi_col,
                       uint8_t lq_row, uint8_t lq_col) {
    r->rssi_row = rssi_row; r->rssi_col = rssi_col;
    r->lq_row = lq_row; r->lq_col = lq_col;
}

void rx_grid_on_video(RxGrid *r) { r->rx_video_total++; }
void rx_grid_set_vid_rssi(RxGrid *r, int16_t dbm) {
    atomic_store_explicit(&r->vid_rssi_dbm, (int)dbm, memory_order_relaxed);
}

int rx_grid_on_packet(RxGrid *r, const uint8_t *payload, size_t len) {
    osd_grid_t decoded; uint32_t tx_total = 0;
    int rc = osd_grid_reasm_feed(&r->reasm, payload, len, &decoded, &tx_total);
    if (rc != 1) return rc;             /* 0 partial/dropped, -1 malformed */

    /* vid LQ over a sliding ~1 s window of packet-count deltas. Averaging across
     * LQ_SMOOTH_KEYFRAMES keyframes (not per keyframe) cancels the TX-timer vs
     * RX-reassembly-clock window mismatch: a keyframe delayed behind a video
     * burst makes its own window long (rx_delta high) and the next window short
     * (rx_delta low); summed, the two cancel instead of flickering LQ to ~55.
     * Deltas are summed raw -- rx_delta may exceed tx_delta in a catch-up window
     * -- and clamped only on the final ratio, so the surplus cancels the prior
     * deficit rather than being clipped away. */
    uint32_t tx_delta = tx_total - r->prev_tx_total;
    uint32_t rx_delta = r->rx_video_total - r->prev_rx_total;
    r->prev_tx_total = tx_total;
    r->prev_rx_total = r->rx_video_total;
    if (!r->lq_primed) {
        /* First keyframe: no prior counter to diff against -- seed the prevs
         * (done above) and report N/A until the next keyframe yields a real
         * delta, so the initial garbage delta never enters the window. */
        r->lq_primed = true;
    } else {
        r->lq_tx_sum -= r->lq_tx_hist[r->lq_pos];
        r->lq_rx_sum -= r->lq_rx_hist[r->lq_pos];
        r->lq_tx_hist[r->lq_pos] = tx_delta;
        r->lq_rx_hist[r->lq_pos] = rx_delta;
        r->lq_tx_sum += tx_delta;
        r->lq_rx_sum += rx_delta;
        r->lq_pos = (uint8_t)((r->lq_pos + 1u) % LQ_SMOOTH_KEYFRAMES);
    }
    if (r->lq_tx_sum == 0)                 r->vid_lq_pct = OSD_VID_LQ_NA;
    else if (r->lq_rx_sum >= r->lq_tx_sum) r->vid_lq_pct = 100;
    else r->vid_lq_pct = (uint8_t)(((uint64_t)r->lq_rx_sum * 100u) / r->lq_tx_sum);

    int16_t dbm = (int16_t)atomic_load_explicit(&r->vid_rssi_dbm, memory_order_relaxed);
    osd_grid_inject_vid(&decoded, r->rssi_row, r->rssi_col, r->lq_row, r->lq_col,
                        dbm, r->vid_lq_pct);

    wr_begin(r);
    r->grid = decoded;
    wr_end(r);
    return 1;
}

int16_t rx_grid_vid_rssi(const RxGrid *r) {
    return (int16_t)atomic_load_explicit(&((RxGrid *)r)->vid_rssi_dbm,
                                         memory_order_relaxed);
}

uint8_t rx_grid_vid_lq(const RxGrid *r) {
    return r->vid_lq_pct;
}

void rx_grid_health(const RxGrid *r, uint64_t *frags_rx, uint64_t *frames_done,
                    uint64_t *frames_dropped, uint64_t *frags_stale) {
    /* Racy read of the reasm counters from the main thread vs the rx_loop
     * writer; benign staleness-tolerant gauges, matching the prior pattern. */
    if (frags_rx)       *frags_rx       = r->reasm.frags_rx;
    if (frames_done)    *frames_done    = r->reasm.frames_done;
    if (frames_dropped) *frames_dropped = r->reasm.frames_dropped;
    if (frags_stale)    *frags_stale    = r->reasm.frags_stale;
}

void rx_grid_read(const RxGrid *r, osd_grid_t *out) {
    unsigned s0, s1;
    do {
        /* Cast away const to load _Atomic through a const pointer — standard C11
         * workaround; logically non-mutating.  Mirrors msp_telemetry.c. */
        s0 = atomic_load_explicit(&((RxGrid *)r)->seq, memory_order_acquire);
        if (s0 & 1u) continue;                 /* write in progress */
        atomic_thread_fence(memory_order_acquire);
        *out = r->grid;
        atomic_thread_fence(memory_order_acquire);
        s1 = atomic_load_explicit(&((RxGrid *)r)->seq, memory_order_acquire);
    } while (s0 != s1 || (s0 & 1u));
}
