/* Host tests for the RX multi-packet stream walker
 * (drivers/lr2021/lr2021_rx_walk.h).
 *
 * WHY THIS EXISTS (RCA 2026-07-05, HW-confirmed with the COALESCE_PROBE):
 * RX_DONE is a single coalescing IRQ status bit and GetRxPktLength returns the
 * length of the LAST received payload (DS 7.3.10). When a second packet
 * finishes while the host is still servicing the previous one, the old drain
 * path read the head-of-FIFO packet with the LAST packet's length -- misframing
 * the head packet (HW: pkt_len=5 trailer reported while a 511 B tail slice sat
 * drained in the accumulator; ~1:1 with damaged AUs at 1.19/s trailer-on).
 *
 * The walker treats accumulator+FIFO as a byte stream and frames each packet
 * by its OWN in-band header (video/FEC: flags + LE16 payload len; the FIXED
 * protocol format is self-framing), using the chip pkt_len only for the
 * classic exact-match/stray disambiguation. Completeness is level-gated by the
 * caller: packets are serial on air, so a head packet is fully present iff
 * acc + fifo_level >= its extent.
 *
 * Pure logic, host-tested here; the kernel adds only SPI reads + ring pushes. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/lr2021/lr2021_rx_walk.h"

/* --- packet builders ------------------------------------------------------ */

/* Video/FEC FIXED-format packet: flags(1) + LE16 payload len(2) + payload.
 * total = full on-wire packet length (payload = total - 3). */
static __u16 mkvid(__u8 *out, __u16 total, __u8 flags, __u8 fill) {
    __u16 pay = (__u16)(total - 3);
    out[0] = flags;                 /* bit0 (TYPE) must be 0 for video/FEC */
    out[1] = (__u8)(pay & 0xFF);
    out[2] = (__u8)(pay >> 8);
    memset(out + 3, fill, pay);
    return total;
}

/* Frame trailer as seen on air: 5-byte video packet (payload = 2 B filler). */
static __u16 mktrailer(__u8 *out) { return mkvid(out, 5, 0xC0, 0x0C); }

/* OSD-grid fragment record: flags 0xC1 (START|END|TYPE) + version + frag hdr.
 * No LE16 -- the walker cannot frame it and must return LEGACY. */
static __u16 mkosd(__u8 *out, __u16 total) {
    out[0] = 0xC1;
    out[1] = 0x01;                  /* OSD_GRID_VERSION */
    out[2] = 0x07;                  /* frame id (chosen != version bytes) */
    memset(out + 3, 0x33, (size_t)(total - 3));
    return total;
}

/* --- single-packet fast paths (must be byte-identical to old behavior) ----- */

static void test_single_clean(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    mkvid(buf, 250, 0x80, 0xAB);
    assert(lr2021_rx_walk_step(buf, 250, 250, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 250);
}

static void test_single_partial_predrain(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    mkvid(buf, 250, 0x80, 0xAB);
    /* only 128 B drained so far; chip says 250 -> need the rest */
    assert(lr2021_rx_walk_step(buf, 128, 250, &off, &n) == LR2021_WALK_NEED);
    assert(n == 122);
}

static void test_single_stray(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    buf[0] = 0xAA;                          /* stray byte (bit0 set: off0 invalid) */
    mkvid(buf + 1, 250, 0x80, 0xAB);
    /* stray packet occupies [1..251): with 251 accumulated it emits at off 1 */
    assert(lr2021_rx_walk_step(buf, 251, 250, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 1 && n == 250);
    /* with exactly pkt_len accumulated, the last real byte is still in the
     * FIFO -> NEED(1) (this replaces the old hand-rolled stray-tail read) */
    assert(lr2021_rx_walk_step(buf, 250, 250, &off, &n) == LR2021_WALK_NEED);
    assert(n == 1);
}

static void test_fec_flags_walk(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    mkvid(buf, 200, 0x02, 0x5A);            /* PROTO_FLAG_FEC set, TYPE clear */
    assert(lr2021_rx_walk_step(buf, 200, 200, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 200);
}

/* --- the coalesce family (the bug) ----------------------------------------- */

static void test_trailer_coalesce_partial_drain(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    mkvid(buf, 250, 0x80, 0x11);
    /* head video packet 250 B, only 134 drained; the 5 B trailer finished too,
     * so the chip reports pkt_len=5. Old code emitted 5 head bytes as a bogus
     * packet and lost the tail slice. Walker: frame the head by ITS OWN LE16. */
    assert(lr2021_rx_walk_step(buf, 134, 5, &off, &n) == LR2021_WALK_NEED);
    assert(n == 116);
    /* after the caller tops up to 250: emit the full head packet */
    assert(lr2021_rx_walk_step(buf, 250, 5, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 250);
}

static void test_trailer_at_head(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    mktrailer(buf);
    /* after emitting the big head the caller continues the walk: 4 header
     * bytes read, trailer's own LE16 (=2) matches pkt_len-3 -> one more byte */
    assert(lr2021_rx_walk_step(buf, 4, 5, &off, &n) == LR2021_WALK_NEED);
    assert(n == 1);
    assert(lr2021_rx_walk_step(buf, 5, 5, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 5);
}

static void test_video_video_coalesce_fully_drained(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    __u16 a = mkvid(buf, 274, 0x80, 0x22);
    mkvid(buf + a, 180, 0x84, 0x33);
    /* both packets in the accumulator; chip reports the LAST (180) */
    assert(lr2021_rx_walk_step(buf, 454, 180, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 274);
    /* caller consumes 274 and steps again on the remainder */
    assert(lr2021_rx_walk_step(buf + 274, 180, 180, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 180);
}

static void test_equal_length_coalesce(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    __u16 a = mkvid(buf, 250, 0x80, 0x44);
    mkvid(buf + a, 250, 0x84, 0x55);
    /* pkt_len==head length AND both drained: v0 exact-match frames the head;
     * walker emits it, remainder walked next -> both delivered */
    assert(lr2021_rx_walk_step(buf, 500, 250, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 250);
    assert(lr2021_rx_walk_step(buf + 250, 250, 250, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 250);
}

static void test_three_packet_coalesce(void) {
    __u8 buf[900];
    int off = -1; __u16 n = 0;
    __u16 a = mkvid(buf, 200, 0x80, 0x66);
    __u16 b = mkvid(buf + a, 150, 0x84, 0x77);
    mktrailer(buf + a + b);
    assert(lr2021_rx_walk_step(buf, 355, 5, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 200);
    assert(lr2021_rx_walk_step(buf + 200, 155, 5, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 150);
    assert(lr2021_rx_walk_step(buf + 350, 5, 5, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 5);
}

/* --- over-drain residue (during-RX drain crossed a packet boundary) -------- */

static void test_overdrain_residue(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    __u16 a = mkvid(buf, 145, 0x80, 0x88);
    __u8 next[600];
    mkvid(next, 250, 0x84, 0x99);
    memcpy(buf + a, next, 10);              /* 10 successor bytes over-drained */
    /* v0 exact-match on the head (145): emit it; residue stays accumulated */
    assert(lr2021_rx_walk_step(buf, 155, 145, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 145);
    /* the residue is the successor's head: in-band framed, NEED its tail.
     * Caller finds level < n (in-flight) and preserves the residue. */
    assert(lr2021_rx_walk_step(buf + 145, 10, 145, &off, &n) == LR2021_WALK_NEED);
    assert(n == 240);
}

/* --- header top-up requests ------------------------------------------------ */

static void test_header_topup(void) {
    __u8 buf[8] = { 0 };
    int off = -1; __u16 n = 0;
    /* nothing accumulated: ask for a parseable header (caller level-gates) */
    assert(lr2021_rx_walk_step(buf, 0, 250, &off, &n) == LR2021_WALK_NEED);
    assert(n == 4);
    mktrailer(buf);
    assert(lr2021_rx_walk_step(buf, 2, 250, &off, &n) == LR2021_WALK_NEED);
    assert(n == 2);
}

/* --- fallbacks: DYN records and garbage ------------------------------------ */

static void test_osd_head_legacy(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    mkosd(buf, 216);
    /* OSD-grid record carries no in-band length: walker must hand back to the
     * legacy pkt_len path (which handles the common single-OSD case exactly
     * as before, including the DYN-signature offset logic). */
    assert(lr2021_rx_walk_step(buf, 216, 216, &off, &n) == LR2021_WALK_LEGACY);
}

static void test_osd_behind_video(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    __u16 a = mkvid(buf, 300, 0x80, 0xAA);
    mkosd(buf + a, 26);
    assert(lr2021_rx_walk_step(buf, 326, 26, &off, &n) == LR2021_WALK_EMIT);
    assert(off == 0 && n == 300);
    /* remainder is the OSD record; pkt_len (26) describes it, LEGACY path
     * emits it correctly (acc == pkt_len) */
    assert(lr2021_rx_walk_step(buf + 300, 26, 26, &off, &n) == LR2021_WALK_LEGACY);
}

static void test_garbage_legacy(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    memset(buf, 0xFF, sizeof(buf));         /* bit0 set everywhere, LE16 huge */
    assert(lr2021_rx_walk_step(buf, 250, 250, &off, &n) == LR2021_WALK_LEGACY);
}

static void test_le16_over_max_payload_legacy(void) {
    __u8 buf[600];
    int off = -1; __u16 n = 0;
    /* flags plausible but LE16 = 509 > PROTOCOL_MAX_PAYLOAD(508): not a real
     * in-band header -> LEGACY, never a 512-byte NEED */
    buf[0] = 0x80; buf[1] = 0xFD; buf[2] = 0x01; buf[3] = 0x00;
    assert(lr2021_rx_walk_step(buf, 4, 250, &off, &n) == LR2021_WALK_LEGACY);
}

int main(void) {
    test_single_clean();
    test_single_partial_predrain();
    test_single_stray();
    test_fec_flags_walk();
    test_trailer_coalesce_partial_drain();
    test_trailer_at_head();
    test_video_video_coalesce_fully_drained();
    test_equal_length_coalesce();
    test_three_packet_coalesce();
    test_overdrain_residue();
    test_header_topup();
    test_osd_head_legacy();
    test_osd_behind_video();
    test_garbage_legacy();
    test_le16_over_max_payload_legacy();
    printf("test_rx_walk: all tests passed\n");
    return 0;
}
