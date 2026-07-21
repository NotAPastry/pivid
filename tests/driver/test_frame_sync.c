/* Host tests for the RX SPI-frame self-sync logic
 * (drivers/lr2021/lr2021_frame_sync.h).
 *
 * The kernel direct-read FIFO frame may carry a stray leading byte (a 16 MHz
 * SPI timing artifact -- see project_rx_misframe_fec_fabrication). These pure
 * helpers decide, per packet, whether the real protocol frame starts at
 * offset 0 (nominal) or 1 (stray prepended). We exercise them on the host;
 * the kernel only adds the dev-> state wiring.
 *
 * Header invariant under test: flags bit 0 is the ONLY reserved bit (must be
 * 0); bit 1 is PROTO_FLAG_FEC and may legitimately be 1. The old driver used
 * (flags & 0x03)==0, which would misframe FEC packets -- these tests pin the
 * corrected 0x01 invariant. The length check is a SINGLE byte (<= 253) by
 * design: it must validate both pivid-linktest's 1-byte length and the
 * protocol's LE16 low byte (see the LR2021_PROTO_MAX_PAYLOAD note). */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/lr2021/lr2021_frame_sync.h"
#include "protocol.h"   /* TELEMETRY_BYTES / TELEMETRY_VERSION */
#include "osd_grid.h"   /* OSD_GRID_VERSION */

/* Cross-boundary guard: the driver header mirrors the telemetry wire signature
 * (it can't include the userspace protocol.h). If the wire format changes in
 * protocol.h without updating the mirror, the RX driver stops recognizing the
 * telemetry frame and ALL telemetry is silently dropped (HW-observed: a 34->35
 * byte / v1->v2 bump that missed this mirror -> msp_rx_ok=0). These asserts make
 * `make tests` fail loudly on drift instead. */
_Static_assert(LR2021_TELEM_PKT_LEN == (unsigned)TELEMETRY_BYTES + 1u,
               "LR2021_TELEM_PKT_LEN must equal TELEMETRY_BYTES + 1 (flags byte)");
_Static_assert(LR2021_TELEM_VERSION == TELEMETRY_VERSION,
               "LR2021_TELEM_VERSION must equal TELEMETRY_VERSION");
_Static_assert(LR2021_OSD_GRID_VERSION == OSD_GRID_VERSION,
               "LR2021_OSD_GRID_VERSION mirror must equal OSD_GRID_VERSION");

/* --- validity helper ------------------------------------------------------ */

static void test_offset_valid_basic(void) {
    /* flags bit0=0, len<=253 -> valid header. */
    __u8 ok[3] = { 0x80, 100, 0 };          /* START flag, len 100 */
    assert(lr2021_frame_offset_valid(ok, 0) == 1);

    /* flags bit0 set -> reserved bit violated -> invalid. */
    __u8 resv[3] = { 0x01, 100, 0 };
    assert(lr2021_frame_offset_valid(resv, 0) == 0);

    /* len > 253 (the single-byte ceiling) -> invalid. */
    __u8 toolong[3] = { 0x00, 254, 0 };
    assert(lr2021_frame_offset_valid(toolong, 0) == 0);
    __u8 max255[3] = { 0x00, 255, 0 };
    assert(lr2021_frame_offset_valid(max255, 0) == 0);

    /* boundary: len == 253 is the max valid. */
    __u8 boundary[3] = { 0x00, 253, 0 };
    assert(lr2021_frame_offset_valid(boundary, 0) == 1);
}

/* The corrected invariant must ACCEPT a FEC-flagged header (bit 1 set).
 * Regression guard against the old (flags & 0x03)==0 check. */
static void test_fec_flagged_header_is_valid(void) {
    /* START|FEC = 0x80|0x02 = 0x82; bit0 still 0 -> must be valid. */
    __u8 fec[3] = { 0x82, 120, 0 };
    assert(lr2021_frame_offset_valid(fec, 0) == 1);
    /* pure FEC bit. */
    __u8 fec_only[3] = { 0x02, 10, 0 };
    assert(lr2021_frame_offset_valid(fec_only, 0) == 1);
}

/* --- per-packet offset decision ------------------------------------------ */

static void test_unambiguous_offset0(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    /* off0 valid (0x80,len100); off1 invalid (p[2]=255 -> len 255). */
    __u8 p[3] = { 0x80, 100, 255 };
    assert(lr2021_frame_sync_offset(&s, p) == 0);
    assert(s.off0_votes == 1 && s.off1_votes == 0);
}

static void test_unambiguous_offset1_stray(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    /* stray 0x01 makes off0 invalid; real header at off1 (0x40,len50). */
    __u8 p[3] = { 0x01, 0x40, 50 };
    assert(lr2021_frame_sync_offset(&s, p) == 1);
    assert(s.off1_votes == 1 && s.off0_votes == 0);
}

static void test_ambiguous_follows_sticky(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);

    /* Make offset 0 the learned-dominant via unambiguous off0 packets. */
    __u8 clear0[3] = { 0x80, 100, 255 };
    for (int i = 0; i < 5; i++) assert(lr2021_frame_sync_offset(&s, clear0) == 0);
    assert(s.off0_votes == 5 && s.off1_votes == 0);

    /* Ambiguous packet (both offsets validate) -> follow sticky (0). */
    __u8 ambig[3] = { 0x00, 0x00, 10 };  /* off0: 0x00,len0 ok; off1: 0x00,len10 ok */
    assert(lr2021_frame_offset_valid(ambig, 0) == 1);
    assert(lr2021_frame_offset_valid(ambig, 1) == 1);
    assert(lr2021_frame_sync_offset(&s, ambig) == 0);
    /* Ambiguous packets must NOT cast a vote. */
    assert(s.off0_votes == 5 && s.off1_votes == 0);
}

static void test_ambiguous_follows_sticky_offset1(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    /* Learn dominant = 1. */
    __u8 clear1[3] = { 0x01, 0x40, 50 };
    for (int i = 0; i < 5; i++) assert(lr2021_frame_sync_offset(&s, clear1) == 1);
    assert(s.off1_votes == 5);

    __u8 ambig[3] = { 0x00, 0x00, 10 };
    assert(lr2021_frame_sync_offset(&s, ambig) == 1);   /* follows sticky 1 */
}

static void test_neither_valid_follows_sticky(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    /* default sticky is 0 (no votes). Both header candidates have a reserved
     * bit set: off0 via p[0] bit0, off1 via p[1] bit0. */
    __u8 bad[3] = { 0x01, 0x01, 0x00 };
    assert(lr2021_frame_offset_valid(bad, 0) == 0);
    assert(lr2021_frame_offset_valid(bad, 1) == 0);
    assert(lr2021_frame_sync_offset(&s, bad) == 0);
    assert(s.off0_votes == 0 && s.off1_votes == 0);   /* no vote */
}

/* Realistic stream: dominant offset 0, occasional UNAMBIGUOUS stray (offset 1).
 * Every packet must be framed correctly per-packet (the per-load lock could
 * not do this -- it was the bug). */
static void test_mixed_stream_per_packet(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    __u8 normal[3] = { 0x84, 40, 255 };   /* off0 only */
    __u8 stray[3]  = { 0x01, 0x44, 30 };  /* off1 only */
    for (int i = 0; i < 100; i++) {
        if (i % 13 == 0) assert(lr2021_frame_sync_offset(&s, stray) == 1);
        else             assert(lr2021_frame_sync_offset(&s, normal) == 0);
    }
}

/* --- DYNAMIC-record signature offset disambiguation ----------------------- */
/* A DYNAMIC pivid record (OSD-grid keyframe fragment, or the retired value
 * telemetry record) has no LE16 length, so the video pkt_len check can't frame
 * it and the 1-byte heuristic rejects it (the TYPE bit trips the reserved-bit-0
 * test). lr2021_dyn_frame_offset disambiguates by the flags+version signature:
 * flags==0xC1, payload[0] in {OSD_GRID_VERSION(1), TELEM_VERSION(2)}. It is
 * LENGTH-AGNOSTIC -- OSD-grid fragments are variable length (the prior pkt_len
 * gate was the 100% OSD-grid misframe). Uses the macros (not literals) so the
 * test tracks the mirrored constants; the _Static_asserts above pin them to the
 * real wire formats. */
static void test_dyn_offset(void) {
    int off;
    const __u16 TL = LR2021_TELEM_PKT_LEN;
    const __u8  TV = LR2021_TELEM_VERSION;
    const __u8  GV = LR2021_OSD_GRID_VERSION;

    /* telemetry, offset 0 (no stray): [C1][ver=2][fc=00][...] -> off 0. */
    __u8 p0[40] = { 0xC1, TV, 0x00, 0x03 };
    off = -1;
    assert(lr2021_dyn_frame_offset(p0, TL, &off) == 1 && off == 0);

    /* telemetry, offset 1 (stray prepended): [stray][C1][ver=2][...] -> off 1. */
    __u8 p1[40] = { 0xAB, 0xC1, TV, 0x00 };
    off = -1;
    assert(lr2021_dyn_frame_offset(p1, TL, &off) == 1 && off == 1);

    /* OSD-grid fragment, offset 0: [C1][ver=1][frame_id][frag_idx]... at a
     * large variable pkt_len that the old pkt_len==36 gate rejected -> off 0.
     * This is the case that was 100% misframing (osd_frags_rx=0). */
    __u8 g0[260] = { 0xC1, GV, 0x07, 0x00, 0x01 };
    off = -1;
    assert(lr2021_dyn_frame_offset(g0, 217, &off) == 1 && off == 0);

    /* OSD-grid fragment, offset 1 (stray): [stray][C1][ver=1][frame_id]... */
    __u8 g1[260] = { 0x5A, 0xC1, GV, 0x07, 0x00 };
    off = -1;
    assert(lr2021_dyn_frame_offset(g1, 217, &off) == 1 && off == 1);

    /* video flags (bit0=0, never 0xC1), any length -> not a DYNAMIC record. */
    __u8 vid[40] = { 0x80, 0x20, 0x00, 0x00 };
    assert(lr2021_dyn_frame_offset(vid, TL, &off) == 0);
    assert(lr2021_dyn_frame_offset(vid, 217, &off) == 0);

    /* 0xC1 but an unknown version byte -> not a recognized record. */
    __u8 badver[40] = { 0xC1, 0x09, 0x00, 0x00 };
    assert(lr2021_dyn_frame_offset(badver, 217, &off) == 0);

    /* neither offset carries the signature -> not resolved (caller falls back). */
    __u8 none[40] = { 0x55, 0x66, 0x77, 0x88 };
    assert(lr2021_dyn_frame_offset(none, TL, &off) == 0);
}

/* --- classification (diagnostic instrument) ------------------------------- */
/* lr2021_frame_sync_classify reports WHICH decision category each packet fell
 * into so the kernel can count strays (FC_OFF1) and risky ambiguous decisions
 * (FC_AMBIG) without changing framing behavior. The chosen offset MUST be
 * identical to lr2021_frame_sync_offset for every input (behavior-preserving). */

static void test_classify_unambiguous_off0(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    __u8 p[3] = { 0x80, 100, 255 };          /* off0 only */
    int off = -1;
    assert(lr2021_frame_sync_classify(&s, p, &off) == LR2021_FC_OFF0);
    assert(off == 0);
    assert(s.off0_votes == 1 && s.off1_votes == 0);   /* vote as before */
}

static void test_classify_unambiguous_off1(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    __u8 p[3] = { 0x01, 0x40, 50 };          /* off1 only (stray) */
    int off = -1;
    assert(lr2021_frame_sync_classify(&s, p, &off) == LR2021_FC_OFF1);
    assert(off == 1);
    assert(s.off1_votes == 1 && s.off0_votes == 0);
}

static void test_classify_ambiguous_both_valid(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    /* Learn dominant 0 first. */
    __u8 clear0[3] = { 0x80, 100, 255 };
    for (int i = 0; i < 5; i++) (void)lr2021_frame_sync_offset(&s, clear0);
    __u8 ambig[3] = { 0x00, 0x00, 10 };      /* both offsets validate */
    int off = -1;
    assert(lr2021_frame_sync_classify(&s, ambig, &off) == LR2021_FC_AMBIG);
    assert(off == 0);                         /* follows dominant */
    assert(s.off0_votes == 5 && s.off1_votes == 0);   /* no vote */
}

static void test_classify_ambiguous_neither_valid(void) {
    struct lr2021_frame_sync s; memset(&s, 0, sizeof s);
    __u8 bad[3] = { 0x01, 0x01, 0x00 };      /* neither offset validates */
    int off = -1;
    assert(lr2021_frame_sync_classify(&s, bad, &off) == LR2021_FC_AMBIG);
    assert(off == 0);
}

/* Equivalence: classify's chosen offset == lr2021_frame_sync_offset for the
 * SAME state evolution across a representative stream. */
static void test_classify_offset_equivalence(void) {
    struct lr2021_frame_sync a; memset(&a, 0, sizeof a);
    struct lr2021_frame_sync b; memset(&b, 0, sizeof b);
    __u8 normal[3] = { 0x84, 40, 255 };
    __u8 stray[3]  = { 0x01, 0x44, 30 };
    __u8 ambig[3]  = { 0x00, 0x00, 10 };
    for (int i = 0; i < 200; i++) {
        __u8 *p = (i % 7 == 0) ? stray : (i % 11 == 0) ? ambig : normal;
        int off_c = -1;
        (void)lr2021_frame_sync_classify(&a, p, &off_c);
        int off_o = lr2021_frame_sync_offset(&b, p);
        assert(off_c == off_o);
    }
    /* And the vote tallies must evolve identically. */
    assert(a.off0_votes == b.off0_votes && a.off1_votes == b.off1_votes);
}

int main(void) {
    test_offset_valid_basic();
    test_fec_flagged_header_is_valid();
    test_unambiguous_offset0();
    test_unambiguous_offset1_stray();
    test_ambiguous_follows_sticky();
    test_ambiguous_follows_sticky_offset1();
    test_neither_valid_follows_sticky();
    test_mixed_stream_per_packet();
    test_dyn_offset();
    test_classify_unambiguous_off0();
    test_classify_unambiguous_off1();
    test_classify_ambiguous_both_valid();
    test_classify_ambiguous_neither_valid();
    test_classify_offset_equivalence();
    printf("All frame-sync tests passed!\n");
    return 0;
}
