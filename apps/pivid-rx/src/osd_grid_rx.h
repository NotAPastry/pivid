/* apps/pivid-rx/src/osd_grid_rx.h -- RX-side grid: seqlock-published shadow
 * grid fed by reassembled keyframes, masked vid RSSI/LQ injection, and
 * receive-health counters. Single writer (rx_loop thread), lock-free reader
 * (telem_tx thread). Mirrors the msp_telemetry seqlock pattern it replaces. */
#ifndef RX_OSD_GRID_RX_H
#define RX_OSD_GRID_RX_H
#include "osd_grid.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* ~1 s of vid-LQ averaging at the default 200 ms (5 Hz) keyframe cadence. The
 * LQ ratio is taken over a sliding window of this many per-keyframe packet
 * deltas rather than per keyframe, so the TX-timer vs RX-reassembly window
 * mismatch (a keyframe delayed behind a video burst, then the next arriving
 * early) cancels instead of flickering LQ to 50-60. Scales with the cadence. */
#define LQ_SMOOTH_KEYFRAMES 5u

typedef struct {
    atomic_uint      seq;            /* even=stable, odd=writing */
    osd_grid_t       grid;          /* last good grid, post-injection */
    osd_grid_reasm_t reasm;
    /* injection inputs */
    _Atomic int      vid_rssi_dbm;
    uint32_t         rx_video_total, prev_tx_total, prev_rx_total;
    uint8_t          vid_lq_pct;
    /* vid-LQ sliding-window smoothing (see LQ_SMOOTH_KEYFRAMES). Raw per-keyframe
     * (tx,rx) packet deltas; the ratio is taken over the running sums so a
     * catch-up window's rx>tx surplus cancels the preceding window's deficit
     * instead of being clipped away by the 100 clamp. */
    uint32_t         lq_tx_hist[LQ_SMOOTH_KEYFRAMES], lq_rx_hist[LQ_SMOOTH_KEYFRAMES];
    uint32_t         lq_tx_sum, lq_rx_sum;
    uint8_t          lq_pos;
    bool             lq_primed;
    /* reserved cells (from config) */
    uint8_t          rssi_row, rssi_col, lq_row, lq_col;
} RxGrid;

void rx_grid_init(RxGrid *r, uint8_t rssi_row, uint8_t rssi_col,
                  uint8_t lq_row, uint8_t lq_col);
/* Relocate the injected RSSI/LQ cells live (SIGHUP reload). Plain byte stores:
 * the rx_loop writer reads these once per completed frame in rx_grid_on_packet,
 * so a concurrent update can at worst place one frame's icons at mixed old/new
 * coords -- benign and self-correcting on the next frame. No restart needed. */
void rx_grid_set_cells(RxGrid *r, uint8_t rssi_row, uint8_t rssi_col,
                       uint8_t lq_row, uint8_t lq_col);
/* Feed one PKT_OSD_GRID payload (off the radio). On a completed frame,
 * injects vid metrics and publishes the grid. Returns 1 published, 0 partial/
 * dropped, -1 malformed. */
int  rx_grid_on_packet(RxGrid *r, const uint8_t *payload, size_t len);
void rx_grid_on_video(RxGrid *r);                 /* bump rx_video_total */
void rx_grid_set_vid_rssi(RxGrid *r, int16_t dbm);
void rx_grid_read(const RxGrid *r, osd_grid_t *out);

/* Read-only gauges for the status file (written by the rx_loop thread; read by
 * the main thread without the seqlock -- a benign staleness-tolerant gauge). */
int16_t rx_grid_vid_rssi(const RxGrid *r);   /* current injected vid RSSI (dBm) */
uint8_t rx_grid_vid_lq(const RxGrid *r);     /* last computed vid LQ % (or OSD_VID_LQ_NA) */
void    rx_grid_health(const RxGrid *r, uint64_t *frags_rx, uint64_t *frames_done,
                       uint64_t *frames_dropped, uint64_t *frags_stale);
#endif
