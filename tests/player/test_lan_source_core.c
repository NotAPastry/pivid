#include "lan_source.h"
#include "lanfrag.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Build a single-chunk lanfrag datagram carrying one framed NAL. payload_len is
 * implicit at parse (dg_len - LANFRAG_HDR_BYTES), so a one-chunk NAL uses
 * frag_off=0 and total_len=nal_len. */
static size_t mk_dg(uint8_t *dg, uint16_t nal_id, const uint8_t *nal, size_t nal_len) {
    lanfrag_pack_hdr(dg, nal_id, /*frag_off*/0, (uint16_t)nal_len);
    memcpy(dg + LANFRAG_HDR_BYTES, nal, nal_len);
    return LANFRAG_HDR_BYTES + nal_len;
}
static size_t mk_nal(uint8_t *o, uint8_t hdr, uint8_t rbsp0) {
    o[0]=0;o[1]=0;o[2]=0;o[3]=1;o[4]=hdr;o[5]=rbsp0; return 6;
}

#define MS_NS(ms) ((uint64_t)(ms) * 1000000ull)

static void test_datagram_to_au(void) {
    printf("test_lan_source_core: two frames -> two AUs via the core\n");
    LanSource s; atomic_bool stop = 0;
    memset(&s, 0, sizeof s); s.stop = &stop;
    lan_source_reset(&s);
    uint8_t nal[8], dg[64];
    size_t nl, dl;
    /* frame N slice-0 (au start), nal_id 0 */
    nl = mk_nal(nal, 0x41, 0x80); dl = mk_dg(dg, 0, nal, nl);
    lan_source_feed_datagram(&s, dg, dl, MS_NS(1));
    const uint8_t *au; size_t aul;
    assert(lan_source_pop_au(&s, &au, &aul) == 0);   /* not closed yet */
    /* frame N+1 slice-0 (au start), nal_id 1 -> closes N */
    nl = mk_nal(nal, 0x41, 0x80); dl = mk_dg(dg, 1, nal, nl);
    lan_source_feed_datagram(&s, dg, dl, MS_NS(2));
    assert(lan_source_pop_au(&s, &au, &aul) == 1);
    assert(aul == 6 && au[4] == 0x41);
    /* wire idle past the threshold closes N+1 */
    lan_source_on_timeout(&s, MS_NS(2) + s.idle_flush_ns);
    assert(lan_source_pop_au(&s, &au, &aul) == 1);
    printf("  ok\n");
}

/* The idle-flush must be gated on idle_flush_ns: a recv timeout that arrives
 * BEFORE the threshold must NOT split the current (still-arriving) frame. */
static void test_idle_flush_threshold_gating(void) {
    printf("test_lan_source_core: idle-flush gated on idle_flush_ns\n");
    LanSource s; atomic_bool stop = 0;
    memset(&s, 0, sizeof s); s.stop = &stop;
    lan_source_reset(&s);
    s.idle_flush_ns = MS_NS(50);            /* explicit threshold */
    uint8_t nal[8], dg[64];
    size_t nl = mk_nal(nal, 0x41, 0x80);    /* one au-start VCL NAL (a frame) */
    size_t dl = mk_dg(dg, 0, nal, nl);
    lan_source_feed_datagram(&s, dg, dl, MS_NS(100));   /* last_data_ns = 100ms */
    const uint8_t *au; size_t aul;
    /* timeout 8ms later: below threshold -> no flush */
    lan_source_on_timeout(&s, MS_NS(108));
    assert(lan_source_pop_au(&s, &au, &aul) == 0);
    /* another sub-threshold timeout -> still no flush */
    lan_source_on_timeout(&s, MS_NS(140));
    assert(lan_source_pop_au(&s, &au, &aul) == 0);
    /* timeout at/after threshold (>=150ms) -> flush emits the buffered AU */
    lan_source_on_timeout(&s, MS_NS(150));
    assert(lan_source_pop_au(&s, &au, &aul) == 1);
    assert(aul == 6 && au[4] == 0x41);
    printf("  ok\n");
}

/* idle_flush_ns==0 disables the timeout flush entirely: only the next frame's
 * au-start closes an AU (avformat-equivalent, no premature split). */
static void test_idle_flush_disabled(void) {
    printf("test_lan_source_core: idle_flush_ns==0 never flushes on timeout\n");
    LanSource s; atomic_bool stop = 0;
    memset(&s, 0, sizeof s); s.stop = &stop;
    lan_source_reset(&s);
    s.idle_flush_ns = 0;                    /* disabled */
    uint8_t nal[8], dg[64];
    size_t nl = mk_nal(nal, 0x41, 0x80);
    size_t dl = mk_dg(dg, 0, nal, nl);
    lan_source_feed_datagram(&s, dg, dl, MS_NS(10));
    const uint8_t *au; size_t aul;
    /* many timeouts, arbitrarily far in the future -> never flushes */
    lan_source_on_timeout(&s, MS_NS(1000));
    lan_source_on_timeout(&s, MS_NS(100000));
    assert(lan_source_pop_au(&s, &au, &aul) == 0);
    /* only the next frame's au-start closes the buffered AU */
    dl = mk_dg(dg, 1, nal, nl);
    lan_source_feed_datagram(&s, dg, dl, MS_NS(100001));
    assert(lan_source_pop_au(&s, &au, &aul) == 1);
    printf("  ok\n");
}

/* The DEFAULT idle-flush threshold must sit ABOVE the RX FEC block-hold worst
 * case (~100 ms on still scenes: a k=16 block fills over several small
 * frames, so a frame's tail slices legitimately arrive that late). 50 ms sat
 * inside that window and fired mid-frame, presenting top slices and dropping
 * the bottom ones (bottom-half-corruption RCA, 2026-07-05). With the trailer
 * terminating every AU in au_asm, this is a true-stall fallback only. */
static void test_default_idle_flush_above_fec_hold(void) {
    printf("test_lan_source_core: default idle-flush is 250ms (> FEC hold)\n");
    LanSource s; atomic_bool stop = 0;
    memset(&s, 0, sizeof s); s.stop = &stop;
    lan_source_reset(&s);
    assert(s.idle_flush_ns == 250ull * 1000000ull);
    printf("  ok\n");
}

int main(void) {
    test_datagram_to_au();
    test_idle_flush_threshold_gating();
    test_idle_flush_disabled();
    test_default_idle_flush_above_fec_hold();
    printf("all lan_source_core tests passed\n");
    return 0;
}
