/* Host-side tests for apps/pivid-rx/src/rx_loop.c.
 *
 * Strategy:
 *   - Fake RX_Dev backed by a heap-allocated RADIO_RING_BYTES block (same
 *     layout as the mmap'd kernel ring). The SPSC contract is symmetric, so
 *     pushing test data via ring_user_push and popping via rx_pop works.
 *   - rx_loop_drain_once is unit-tested directly: pre-push records, call
 *     drain, assert the reassembler's emit callback fires with expected
 *     payloads.
 *   - rx_loop_run is exercised only for the wake_fd return-2 path; the
 *     drain side is covered by drain_once tests.
 */

#define _POSIX_C_SOURCE 200809L

#include "../apps/pivid-rx/src/rx_loop.h"
#include "../apps/pivid-rx/src/rx.h"
#include "../apps/pivid-rx/src/reasm.h"
#include "../apps/pivid-rx/src/fec_rx.h"
#include "../apps/pivid-rx/src/output.h"
#include "../apps/pivid-rx/src/osd_grid_rx.h"
#include "ring_user.h"
#include "radio_ioctl.h"
#include "protocol.h"
#include "../common/fec.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* --- FEC-to-reasm shim (used by all tests that wire FecRx into rx_loop). */
static void fec_to_reasm(const uint8_t *record, size_t len, void *user) {
    reasm_feed((Reasm *)user, record, len);
}

/* --- Recording sink for reasm emit callbacks. ------------------------- */
typedef struct {
    uint8_t  bytes[REASM_BUF_BYTES];
    size_t   len;
} Emit_Record;

#define MAX_EMITS 32
static Emit_Record g_emits[MAX_EMITS];
static size_t      g_emit_count = 0;

static void recording_emit(const uint8_t *payload, size_t len, void *user) {
    (void)user;
    if (g_emit_count >= MAX_EMITS) return;
    Emit_Record *rec = &g_emits[g_emit_count++];
    rec->len = len;
    if (len > sizeof rec->bytes) len = sizeof rec->bytes;
    memcpy(rec->bytes, payload, len);
}

static void emits_reset(void) {
    g_emit_count = 0;
    memset(g_emits, 0, sizeof g_emits);
}

/* --- Fake RX_Dev plumbing. -------------------------------------------- */
typedef struct {
    RX_Dev dev;
    int    pipe_r;
    int    pipe_w;
} Fake_Dev;

static void fake_dev_init(Fake_Dev *fd) {
    int p[2];
    if (pipe(p) != 0) { perror("pipe"); exit(1); }
    fd->pipe_r = p[0];
    fd->pipe_w = p[1];
    memset(&fd->dev, 0, sizeof fd->dev);
    fd->dev.fd = fd->pipe_r;
    fd->dev.rx_ring = aligned_alloc(4096, RADIO_RING_BYTES);
    assert(fd->dev.rx_ring);
    memset(fd->dev.rx_ring, 0, RADIO_RING_BYTES);
}

static void fake_dev_destroy(Fake_Dev *fd) {
    free(fd->dev.rx_ring);
    close(fd->pipe_r);
    close(fd->pipe_w);
}

/* Push one protocol packet (built from flags/payload) to the fake ring. */
static void push_packet(Fake_Dev *fd, uint8_t flags,
                        const uint8_t *payload, size_t plen) {
    uint8_t pkt[3 + PROTOCOL_MAX_PAYLOAD];
    int n = protocol_build(pkt, sizeof pkt, PROTO_MODE_FIXED, flags, payload, plen);
    assert(n > 0);
    int rc = ring_user_push(fd->dev.rx_ring, pkt, (size_t)n);
    assert(rc == 0);
}

/* Push one TYPE=1 OSD-grid fragment record (DYNAMIC outer framing, as TX builds
 * it). fec=true sets the (invalid) FEC flag to exercise the corrupt-frame drop. */
static void push_grid_frag(Fake_Dev *fd, const uint8_t *payload, size_t plen,
                           bool fec) {
    uint8_t flags = pivid_pack_flags(false, false, 0, PKT_OSD_GRID);
    if (fec) flags |= PROTO_FLAG_FEC;
    uint8_t pkt[3 + PROTOCOL_MAX_PAYLOAD];
    int n = protocol_build(pkt, sizeof pkt, PROTO_MODE_DYNAMIC, flags, payload, plen);
    assert(n > 0);
    int rc = ring_user_push(fd->dev.rx_ring, pkt, (size_t)n);
    assert(rc == 0);
}

/* --- Helpers for building FEC-coded outer records (mirrors test_fec_rx.c).
 * Data symbols (idx<k) ship truncated to 3+inner-len; parity (idx>=k) full. */
static size_t sym_wire_len(const uint8_t *sym, uint8_t idx, uint8_t k){
    if (idx < k) return (size_t)3 + (size_t)unpack_le16(&sym[1]);
    return FEC_SYMBOL_LEN;
}
static size_t mk_fec(uint8_t *out, uint8_t bid, uint8_t idx, uint8_t k, uint8_t n,
                     const uint8_t *coded) {
    uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
    /* A1: switched to pair header (count out-arg); A2 extends demux. */
    proto_fec_pair_hdr_pack(payload, bid, idx, k, n, 1);
    size_t l = sym_wire_len(coded, idx, k);
    memcpy(payload + FEC_HDR_BYTES, coded, l);
    int len = protocol_build(out, 512, PROTO_MODE_FIXED, PROTO_FLAG_FEC, payload,
                             FEC_HDR_BYTES + l);
    return (size_t)len;
}

/* Build one inner data symbol (the RS symbol's real bytes): [flags][len][pl]. */
static size_t mk_inner(uint8_t *sym, uint8_t flags,
                       const uint8_t *pl, size_t pl_len) {
    memset(sym, 0, FEC_SYMBOL_LEN);
    return (size_t)protocol_build(sym, FEC_SYMBOL_LEN, PROTO_MODE_FIXED, flags, pl, pl_len);
}

/* --- Case 1: drain_once on empty ring --------------------------------- */
static void test_drain_empty(void) {
    emits_reset();
    Fake_Dev fdev; fake_dev_init(&fdev);

    Reasm r; reasm_init(&r, recording_emit, NULL);
    FecRx fec; fec_rx_init(&fec, fec_to_reasm, &r);
    RX_Loop loop; rx_loop_init(&loop, &fdev.dev, &r, &fec, NULL, -1);
    RxGrid grid; rx_grid_init(&grid, 0,0, 1,0); loop.grid = &grid;

    int popped = rx_loop_drain_once(&loop);
    EXPECT(popped == 0, "empty ring drains 0 records");
    EXPECT(g_emit_count == 0, "no emits on empty ring");
    EXPECT(loop.records_popped == 0, "records_popped stat 0");

    fake_dev_destroy(&fdev);
}

/* --- Case 2: drain_once on single-packet NAL -------------------------- */
static void test_drain_single_nal(void) {
    emits_reset();
    Fake_Dev fdev; fake_dev_init(&fdev);

    Reasm r; reasm_init(&r, recording_emit, NULL);
    FecRx fec; fec_rx_init(&fec, fec_to_reasm, &r);
    RX_Loop loop; rx_loop_init(&loop, &fdev.dev, &r, &fec, NULL, -1);
    RxGrid grid; rx_grid_init(&grid, 0,0, 1,0); loop.grid = &grid;

    uint8_t payload[50];
    for (int i = 0; i < 50; i++) payload[i] = (uint8_t)(0x10 + i);
    /* START | END, SEQ=0 -> single-packet train. */
    uint8_t flags = PROTO_FLAG_START | PROTO_FLAG_END | PROTO_FLAG_SEQ(0);
    push_packet(&fdev, flags, payload, sizeof payload);

    int popped = rx_loop_drain_once(&loop);
    EXPECT(popped == 1, "drains 1 record");
    EXPECT(loop.records_popped == 1, "records_popped stat 1");
    EXPECT(g_emit_count == 1, "emit fires once");
    EXPECT(g_emits[0].len == sizeof payload, "emit length matches");
    EXPECT(memcmp(g_emits[0].bytes, payload, sizeof payload) == 0,
           "emit payload bytes match");
    EXPECT(r.nals_emitted == 1, "reasm nals_emitted == 1");
    EXPECT(fdev.dev.packets_received == 1, "rx packets_received == 1");

    fake_dev_destroy(&fdev);
}

/* --- Case 3: drain_once on three-packet train ------------------------- */
static void test_drain_three_packet_train(void) {
    emits_reset();
    Fake_Dev fdev; fake_dev_init(&fdev);

    Reasm r; reasm_init(&r, recording_emit, NULL);
    FecRx fec; fec_rx_init(&fec, fec_to_reasm, &r);
    RX_Loop loop; rx_loop_init(&loop, &fdev.dev, &r, &fec, NULL, -1);
    RxGrid grid; rx_grid_init(&grid, 0,0, 1,0); loop.grid = &grid;

    /* Build a 600-byte NAL spanning three chunks. The multi-fragment START
     * carries the 2-byte LE total-length prefix (total = 600). */
    uint8_t nal[600];
    for (int i = 0; i < 600; i++) nal[i] = (uint8_t)(i & 0xFF);
    uint8_t start_pl[2 + 200];
    pack_le16(start_pl, 600);
    memcpy(start_pl + 2, &nal[0], 200);
    push_packet(&fdev, PROTO_FLAG_START | PROTO_FLAG_SEQ(0), start_pl, sizeof start_pl);
    push_packet(&fdev,                    PROTO_FLAG_SEQ(1), &nal[200], 200);
    push_packet(&fdev, PROTO_FLAG_END   | PROTO_FLAG_SEQ(2), &nal[400], 200);

    int popped = rx_loop_drain_once(&loop);
    EXPECT(popped == 3, "drains 3 records");
    EXPECT(loop.records_popped == 3, "records_popped stat 3");
    EXPECT(g_emit_count == 1, "one reassembled emit");
    EXPECT(g_emits[0].len == 600, "reassembled length 600");
    EXPECT(memcmp(g_emits[0].bytes, nal, 600) == 0,
           "reassembled bytes match original NAL");
    EXPECT(r.nals_emitted == 1, "reasm nals_emitted == 1");
    EXPECT(r.dropped_seq_mismatch == 0, "no seq mismatch");

    fake_dev_destroy(&fdev);
}

/* --- Case 4: drain_once over a burst of single-packet NALs ------------ */
static void test_drain_burst(void) {
    emits_reset();
    Fake_Dev fdev; fake_dev_init(&fdev);

    Reasm r; reasm_init(&r, recording_emit, NULL);
    FecRx fec; fec_rx_init(&fec, fec_to_reasm, &r);
    RX_Loop loop; rx_loop_init(&loop, &fdev.dev, &r, &fec, NULL, -1);
    RxGrid grid; rx_grid_init(&grid, 0,0, 1,0); loop.grid = &grid;

    /* 10 single-packet NALs in a row -- each emits immediately. SEQs are
     * independent for single-packet trains (no continuity check), so use
     * SEQ=0 for all. */
    uint8_t pl[16];
    for (int i = 0; i < 10; i++) {
        memset(pl, (uint8_t)(0xA0 + i), sizeof pl);
        uint8_t flags = PROTO_FLAG_START | PROTO_FLAG_END | PROTO_FLAG_SEQ(0);
        push_packet(&fdev, flags, pl, sizeof pl);
    }

    int popped = rx_loop_drain_once(&loop);
    EXPECT(popped == 10, "drains 10 records in one call");
    EXPECT(loop.records_popped == 10, "records_popped stat 10");
    EXPECT(g_emit_count == 10, "10 emits");

    for (size_t i = 0; i < 10; i++) {
        uint8_t want = (uint8_t)(0xA0 + i);
        EXPECT(g_emits[i].len == 16, "emit length 16");
        EXPECT(g_emits[i].bytes[0] == want && g_emits[i].bytes[15] == want,
               "emit payload fill matches expected");
    }

    EXPECT(r.nals_emitted == 10, "reasm nals_emitted == 10");

    fake_dev_destroy(&fdev);
}

/* --- Case 5: rx_loop_run returns 2 promptly on wake_fd signal --------- */
static void test_run_wake_returns_2(void) {
    emits_reset();
    Fake_Dev fdev; fake_dev_init(&fdev);

    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(wake >= 0);

    Reasm r; reasm_init(&r, recording_emit, NULL);
    FecRx fec; fec_rx_init(&fec, fec_to_reasm, &r);
    RX_Loop loop; rx_loop_init(&loop, &fdev.dev, &r, &fec, NULL, wake);
    RxGrid grid; rx_grid_init(&grid, 0,0, 1,0); loop.grid = &grid;

    /* Pre-signal wake_fd; rx_loop_run should return 2 within one poll
     * iteration (well under the 100 ms poll timeout). */
    uint64_t one = 1;
    ssize_t w = write(wake, &one, sizeof one);
    EXPECT(w == sizeof one, "wake_fd write succeeded");

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = rx_loop_run(&loop);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    EXPECT(rc == 2, "rx_loop_run returns 2 on wake_fd");
    EXPECT(elapsed_ms < 200, "rx_loop_run returns quickly (< 200 ms)");
    EXPECT(loop.poll_wakes >= 1, "at least one poll wake recorded");

    close(wake);
    fake_dev_destroy(&fdev);
}

/* --- Case 6: rx_loop_run returns 3 when stats period has elapsed ------ */
static void test_run_heartbeat_returns_3(void) {
    emits_reset();
    Fake_Dev fdev; fake_dev_init(&fdev);

    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(wake >= 0);

    Reasm r; reasm_init(&r, recording_emit, NULL);
    FecRx fec; fec_rx_init(&fec, fec_to_reasm, &r);
    RX_Loop loop; rx_loop_init(&loop, &fdev.dev, &r, &fec, NULL, wake);
    RxGrid grid; rx_grid_init(&grid, 0,0, 1,0); loop.grid = &grid;

    /* Backdate last_stats_ms by 6 s so the heartbeat check fires on the
     * very first iteration (no real sleep needed). */
    loop.last_stats_ms -= 6000;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = rx_loop_run(&loop);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    EXPECT(rc == 3, "rx_loop_run returns 3 when stats period elapsed");
    EXPECT(elapsed_ms < 300, "heartbeat return doesn't wait a full poll cycle");

    close(wake);
    fake_dev_destroy(&fdev);
}

/* --- Case 7: FEC path wired through drain_once — recovers a lost packet. --
 *
 * Build a k=2, m=1 block (n=3).  Inner packet 0 = START|SEQ(0)|payload A;
 * inner packet 1 = END|SEQ(1)|payload B.  Encode 1 parity symbol.
 * Push the surviving data idx0 + parity idx2 to the ring (DROP idx1).
 * After drain_once (have=2 of n=3, block still open), manually call
 * fec_rx_tick with an absurdly large now_ns to force the timeout close.
 * Reasm should then emit one reassembled NAL = A||B. */
static void test_drain_fec_recovers_lost_packet(void) {
    emits_reset();
    fec_gf_init();

    Fake_Dev fdev; fake_dev_init(&fdev);
    Reasm r; reasm_init(&r, recording_emit, NULL);
    FecRx fec; fec_rx_init(&fec, fec_to_reasm, &r);
    RX_Loop loop; rx_loop_init(&loop, &fdev.dev, &r, &fec, NULL, -1);
    RxGrid grid; rx_grid_init(&grid, 0,0, 1,0); loop.grid = &grid;

    /* Payloads: 50 bytes each. */
    uint8_t payload_a[50], payload_b[50];
    for (int i = 0; i < 50; i++) {
        payload_a[i] = (uint8_t)(0xA0 + i);
        payload_b[i] = (uint8_t)(0xB0 + i);
    }

    /* Build inner symbols. idx0 = START|SEQ(0), idx1 = END|SEQ(1).
     * This is a multi-fragment NAL, so the START symbol carries a 2-byte LE
     * total-length prefix (total = 100) ahead of payload_a. */
    uint8_t sym0[FEC_SYMBOL_LEN], sym1[FEC_SYMBOL_LEN];
    uint8_t start_pl[2 + 50];
    pack_le16(start_pl, 100);
    memcpy(start_pl + 2, payload_a, sizeof payload_a);
    size_t slen0 = mk_inner(sym0, PROTO_FLAG_START | PROTO_FLAG_SEQ(0),
                            start_pl, sizeof start_pl);
    /* slen1 not passed to mk_fec (parity uses FEC_SYMBOL_LEN); suppress warning. */
    (void)mk_inner(sym1, PROTO_FLAG_END | PROTO_FLAG_SEQ(1),
                   payload_b, sizeof payload_b);

    /* Encode 1 parity symbol (m=1). */
    uint8_t par0[FEC_SYMBOL_LEN];
    const uint8_t *dp[2]; uint8_t *pp[1];
    dp[0] = sym0; dp[1] = sym1;
    pp[0] = par0;
    fec_encode(dp, 2, 1, pp);

    /* Build outer FEC records: block_id=42, k=2, n=3.
     * A5: data symbols ship truncated to their inner length (the RX walk
     * zero-extends to FEC_SYMBOL_LEN before RS decode), parity stays full.
     * The host ring uses a 2-byte LE length prefix so it carries either size
     * fine. RS columns stay consistent (RX zero-extends the truncated tail). */
    (void)slen0;
    uint8_t rec0[512], rec2[512];
    size_t rlen0 = mk_fec(rec0, 42, 0, 2, 3, sym0);  /* data idx0 (truncated) */
    size_t rlen2 = mk_fec(rec2, 42, 2, 2, 3, par0);  /* parity idx2 (full) */

    /* Push surviving data (idx0) + parity (idx2) to fake ring. DROP idx1. */
    int rc;
    rc = ring_user_push(fdev.dev.rx_ring, rec0, rlen0);
    assert(rc == 0);
    rc = ring_user_push(fdev.dev.rx_ring, rec2, rlen2);
    assert(rc == 0);

    /* Drain: fec_rx_feed is called for both; have==2 < n==3, no auto-close. */
    int popped = rx_loop_drain_once(&loop);
    EXPECT(popped == 2, "drained 2 FEC outer records");
    EXPECT(g_emit_count == 0, "no NAL emitted yet (block still open)");

    /* Force timeout close. Use UINT64_MAX as now_ns — guaranteed past timeout. */
    fec_rx_tick(&fec, (uint64_t)-1);

    /* After close: RS recovers idx1; reasm sees idx0+idx1 (START+END) -> 1 NAL. */
    EXPECT(g_emit_count == 1, "FEC recovered NAL reaches reasm");
    EXPECT(fec.fec_recovered == 1, "one block recovered");

    /* Reassembled NAL = payload_a || payload_b (100 bytes total). */
    EXPECT(g_emits[0].len == 100, "reassembled NAL length == 100");
    EXPECT(memcmp(g_emits[0].bytes,       payload_a, 50) == 0, "NAL bytes[0..49] == payload_a");
    EXPECT(memcmp(g_emits[0].bytes + 50,  payload_b, 50) == 0, "NAL bytes[50..99] == payload_b");

    fake_dev_destroy(&fdev);
}

static void test_drop_helper_rate(void) {
    uint64_t st = 0; int drops = 0;
    for (int i = 0; i < 100; i++) if (rx_loop_should_drop(&st, 10)) drops++;
    EXPECT(drops == 10, "pct=10 -> 10/100 dropped");
    st = 0; drops = 0;
    for (int i = 0; i < 100; i++) if (rx_loop_should_drop(&st, 0)) drops++;
    EXPECT(drops == 0, "pct=0 -> never drop");
    st = 0; drops = 0;
    for (int i = 0; i < 100; i++) if (rx_loop_should_drop(&st, 100)) drops++;
    EXPECT(drops == 100, "pct=100 -> always drop");
}

/* rx_loop_init must zero last_valid_rx_ns regardless of prior memory. */
static void test_last_valid_rx_ns_zero_init(void) {
    RX_Loop l;
    memset(&l, 0xFF, sizeof l);            /* poison so a missed init shows */
    rx_loop_init(&l, NULL, NULL, NULL, NULL, -1);
    assert(l.last_valid_rx_ns == 0);
    printf("test_last_valid_rx_ns_zero_init: OK\n");
}

static void test_grid_receive_counters(void) {
    emits_reset();
    Fake_Dev fdev; fake_dev_init(&fdev);
    Reasm r; reasm_init(&r, recording_emit, NULL);
    FecRx fec; fec_rx_init(&fec, fec_to_reasm, &r);
    RX_Loop loop; rx_loop_init(&loop, &fdev.dev, &r, &fec, NULL, -1);
    RxGrid grid; rx_grid_init(&grid, 0,0, 1,0); loop.grid = &grid;

    /* Build a small grid (one short string) and encode it to a keyframe. A short
     * string encodes well under OSD_GRID_FRAG_CHUNK, so it fragments to exactly
     * one fragment -> a single push completes the frame. */
    osd_grid_t g; osd_grid_clear(&g);
    const uint8_t txt[] = { 'H','I' };
    osd_grid_write_string(&g, 5, 5, 0, txt, sizeof txt);
    uint8_t kf[OSD_GRID_FRAG_CHUNK * OSD_GRID_MAX_FRAGS];
    int kf_len = osd_grid_encode(&g, kf, sizeof kf);
    EXPECT(kf_len > 0, "grid keyframe encodes");

    uint8_t frags[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX];
    size_t  flens[OSD_GRID_MAX_FRAGS];
    int nfrags = osd_grid_fragment(kf, (size_t)kf_len, /*frame_id=*/7,
                                   /*tx_video_total=*/0, frags, flens);
    EXPECT(nfrags == 1, "short grid is a single fragment");

    /* (a) push the real fragment(s) -> frame completes -> frames_done == 1 */
    for (int i = 0; i < nfrags; i++)
        push_grid_frag(&fdev, frags[i], flens[i], /*fec=*/false);

    rx_loop_drain_once(&loop);

    uint64_t fr, fd, fdr, fs;
    rx_grid_health(&grid, &fr, &fd, &fdr, &fs);
    EXPECT(fd == 1, "one grid keyframe completed");

    /* (b) Isolate the FEC-drop guard. Build a FRESH single-fragment keyframe
     * with a NEWER frame_id (8) and set the (invalid) FEC flag. Because it is a
     * single fragment for a newer frame, absent the FEC guard it WOULD complete
     * and bump frames_done to 2. Re-pushing the old frame_id=7 instead would be
     * vacuous (the reassembler rejects a stale/last-completed frame_id anyway).
     * The FEC flag must be dropped in the TYPE dispatch BEFORE reassembly, so
     * frames_done stays 1. */
    uint8_t frags8[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX];
    size_t  flens8[OSD_GRID_MAX_FRAGS];
    int nfrags8 = osd_grid_fragment(kf, (size_t)kf_len, /*frame_id=*/8,
                                    /*tx_video_total=*/0, frags8, flens8);
    EXPECT(nfrags8 == 1, "newer grid is a single fragment (would complete if not dropped)");
    push_grid_frag(&fdev, frags8[0], flens8[0], /*fec=*/true);
    rx_loop_drain_once(&loop);
    rx_grid_health(&grid, &fr, &fd, &fdr, &fs);
    EXPECT(fd == 1, "FEC-flagged grid fragment does not complete a frame");

    fake_dev_destroy(&fdev);
}

int main(void) {
    test_drop_helper_rate();
    test_drain_empty();
    test_drain_single_nal();
    test_drain_three_packet_train();
    test_drain_burst();
    test_run_wake_returns_2();
    test_run_heartbeat_returns_3();
    test_drain_fec_recovers_lost_packet();
    test_last_valid_rx_ns_zero_init();
    test_grid_receive_counters();
    if (g_failures) {
        fprintf(stderr, "test_rx_loop: %d failures\n", g_failures);
        return 1;
    }
    printf("test_rx_loop: all cases pass\n");
    return 0;
}
