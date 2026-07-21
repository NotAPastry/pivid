#include "osd_grid_rx.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

/* Feed one keyframe carrying cumulative tx_total, after bumping the rx-video
 * counter by rx_inc (the packets "received" in this window). Returns the
 * resulting smoothed vid-LQ. */
static uint8_t lq_after_window(RxGrid *r, const uint8_t *kf, int kn,
                               uint8_t frame_id, uint32_t tx_total, uint32_t rx_inc) {
    for (uint32_t i = 0; i < rx_inc; i++) rx_grid_on_video(r);
    uint8_t f[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t fl[OSD_GRID_MAX_FRAGS];
    int nf = osd_grid_fragment(kf, (size_t)kn, frame_id, tx_total, f, fl);
    int done = 0;
    for (int i = 0; i < nf; i++) done = rx_grid_on_packet(r, f[i], fl[i]);
    assert(done == 1);
    return rx_grid_vid_lq(r);
}

int main(void) {
    RxGrid r; rx_grid_init(&r, 0, 0, 1, 0);
    rx_grid_set_vid_rssi(&r, -55);

    osd_grid_t g; osd_grid_clear(&g);
    const uint8_t s[] = {'H','I'};
    osd_grid_write_string(&g, 5, 5, 0, s, 2);
    uint8_t kf[1200]; int kn = osd_grid_encode(&g, kf, sizeof kf);
    uint8_t f[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX]; size_t fl[OSD_GRID_MAX_FRAGS];
    int nf = osd_grid_fragment(kf, (size_t)kn, 3, 10 /*tx_total*/, f, fl);

    int done = 0;
    for (int i = 0; i < nf; i++) done = rx_grid_on_packet(&r, f[i], fl[i]);
    assert(done == 1);

    osd_grid_t out; rx_grid_read(&r, &out);
    assert(out.cells[5*OSD_GRID_COLS+5].ch == 'H');       /* BF content preserved */
    assert(out.cells[0].ch == OSD_SYM_RSSI);              /* injection present */
    /* First keyframe primes the LQ window (no prior counter to diff) -> vid LQ
     * stays N/A until the next keyframe; the smoothing scenario below exercises
     * the computed path. */

    /* Live cell relocation (SIGHUP path): rx_grid_set_cells moves the injected
     * RSSI/LQ cells; the next completed frame injects at the new coords and no
     * longer at the old ones. */
    rx_grid_set_cells(&r, 3, 4, 5, 6);
    int nf2 = osd_grid_fragment(kf, (size_t)kn, 4, 20, f, fl);
    int done2 = 0;
    for (int i = 0; i < nf2; i++) done2 = rx_grid_on_packet(&r, f[i], fl[i]);
    assert(done2 == 1);
    osd_grid_t out2; rx_grid_read(&r, &out2);
    assert(out2.cells[3*OSD_GRID_COLS+4].ch == OSD_SYM_RSSI);  /* RSSI moved here */
    assert(out2.cells[5*OSD_GRID_COLS+6].ch == OSD_SYM_LQ);    /* LQ moved here */
    assert(out2.cells[0].ch != OSD_SYM_RSSI);                  /* old RSSI cell freed */

    /* --- vid-LQ smoothing: a keyframe delayed behind a video burst makes its
     * own measurement window short (rx_delta low) and the next window long
     * (rx_delta high). Per keyframe this flickers LQ to ~50; the ~1 s sliding
     * average holds it >=85 and the catch-up surplus restores 100. Feed clean
     * windows (tx_delta==rx_delta==80), then a collapsed one (rx_delta=40) and
     * its catch-up (rx_delta=120). --- */
    RxGrid rs; rx_grid_init(&rs, 0, 0, 1, 0);
    uint32_t txt = 0; uint8_t fid = 0; uint8_t lq;
    txt += 80; lq = lq_after_window(&rs, kf, kn, fid++, txt, 80);   /* prime -> N/A */
    assert(lq == OSD_VID_LQ_NA);
    for (int k = 0; k < 4; k++) {                                   /* steady clean link */
        txt += 80; lq = lq_after_window(&rs, kf, kn, fid++, txt, 80);
    }
    assert(lq == 100);                                  /* no false sub-100 floor */
    txt += 80; lq = lq_after_window(&rs, kf, kn, fid++, txt, 40);   /* window collapses */
    assert(lq >= 85);                                   /* smoothed, NOT the raw ~50 */
    txt += 80; lq = lq_after_window(&rs, kf, kn, fid++, txt, 120);  /* catch-up window */
    assert(lq == 100);                                  /* surplus cancels the deficit */

    printf("test_osd_grid_rx: OK\n");
    return 0;
}
