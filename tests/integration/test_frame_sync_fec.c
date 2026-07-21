/* Cross-layer regression test: RX kernel self-sync (lr2021_frame_sync.h) sits
 * BELOW the FEC layer. Two ways this interaction broke before today:
 *   1. FEC packets set flag bit 1 (PROTO_FLAG_FEC); the old self-sync invariant
 *      (flags & 0x03)==0 would have misframed every FEC packet.
 *   2. A stray leading byte (the misframe root cause) must not defeat FEC.
 *
 * This drives the real pipeline order:
 *   fec_tx encode -> simulate RX FIFO framing (optional stray + per-packet
 *   self-sync) -> drop one data packet -> fec_rx decode -> reasm
 * and asserts (a) the self-sync picks the correct offset for every
 * FEC-flagged packet and (b) the anchor NAL is recovered byte-identically.
 * Guards project_rx_misframe_fec_fabrication from regressing once FEC is on. */

#include "../apps/pivid-tx/src/fec_tx.h"
#include "../apps/pivid-rx/src/fec_rx.h"
#include "../apps/pivid-rx/src/reasm.h"
#include "../drivers/lr2021/lr2021_frame_sync.h"
#include "protocol.h"
#include "fec.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define EXPECT(c,m) do{ if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,m);g_failures++;} }while(0)

#define WCAP 512
static uint8_t g_wire[WCAP][512]; static size_t g_wlen[WCAP]; static int g_wn;
static void tx_capture(const uint8_t *p,size_t n,void *u){ (void)u; if(g_wn<WCAP){memcpy(g_wire[g_wn],p,n);g_wlen[g_wn]=n;g_wn++;} }

#define NCAP 32
static uint8_t g_nal[NCAP][REASM_BUF_BYTES]; static size_t g_nlen[NCAP]; static int g_nn;
static void nal_capture(const uint8_t *p,size_t n,void *u){ (void)u; if(g_nn<NCAP){memcpy(g_nal[g_nn],p,n);g_nlen[g_nn]=n;g_nn++;} }
static void fec_to_reasm(const uint8_t *r,size_t n,void *u){ reasm_feed((Reasm*)u,r,n); }

/* TX(m) -> per-packet kernel framing (optional stray + self-sync) -> drop one
 * data packet (wire idx 1) -> RX -> reasm. Verifies the self-sync offset and
 * byte-exact record recovery on every framed packet. */
static void run_framed(int m, const uint8_t *nal, size_t nlen, int prepend_stray) {
    g_wn = 0; g_nn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, m, m, FEC_K_MAX, &seq, PROTO_MODE_FIXED, tx_capture, NULL);
    fec_tx_nal(&tx, nal, nlen);
    fec_tx_flush(&tx);

    struct lr2021_frame_sync sync; memset(&sync, 0, sizeof sync);
    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);

    for (int i = 0; i < g_wn; i++) {
        if (i == 1) continue;                       /* drop one data packet */
        uint8_t buf[513], rec[512];
        size_t L = g_wlen[i];
        if (prepend_stray) { buf[0] = 0x01; memcpy(buf + 1, g_wire[i], L); }
        else               { memcpy(buf, g_wire[i], L); }

        int off = lr2021_frame_sync_offset(&sync, buf);
        EXPECT(off == (prepend_stray ? 1 : 0),
               "self-sync picks the right offset for a FEC-flagged packet");
        memcpy(rec, buf + off, L);
        EXPECT(memcmp(rec, g_wire[i], L) == 0,
               "framed record is byte-identical to the wire FEC packet");
        fec_rx_feed(&rx, rec, L, (uint64_t)(i + 1) * 1000);
    }
    fec_rx_tick(&rx, 0xFFFFFFFFFFFFFFFFull);          /* flush open block */
}

static void test_fec_through_selfsync_nostray(void) {
    uint8_t nal[600]; nal[0] = 0x61; for (int i = 1; i < 600; i++) nal[i] = (uint8_t)(i * 3);
    run_framed(2, nal, sizeof nal, 0);
    EXPECT(g_nn == 1 && g_nlen[0] == sizeof nal && memcmp(g_nal[0], nal, sizeof nal) == 0,
           "FEC recovers anchor through self-sync (no stray)");
}

static void test_fec_through_selfsync_with_stray(void) {
    uint8_t nal[600]; nal[0] = 0x61; for (int i = 1; i < 600; i++) nal[i] = (uint8_t)(i * 3);
    run_framed(2, nal, sizeof nal, 1);
    EXPECT(g_nn == 1 && g_nlen[0] == sizeof nal && memcmp(g_nal[0], nal, sizeof nal) == 0,
           "FEC recovers anchor through self-sync (stray byte on every packet)");
}

int main(void) {
    fec_gf_init();
    test_fec_through_selfsync_nostray();
    test_fec_through_selfsync_with_stray();
    if (g_failures) { fprintf(stderr, "test_frame_sync_fec: %d failure(s)\n", g_failures); return 1; }
    printf("test_frame_sync_fec: OK\n");
    return 0;
}
