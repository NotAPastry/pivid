/* Host tests for apps/pivid-player/src/lan_reasm.c. Build chunk datagrams with
 * lanfrag_pack_hdr, feed them, assert on emitted prefixes + counters. */
#define _POSIX_C_SOURCE 200809L

#include "../apps/pivid-player/src/lan_reasm.h"
#include "lanfrag.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
                   g_failures++; } \
} while (0)

/* emit recorder */
typedef struct { uint8_t bytes[LANFRAG_MAX_NAL]; size_t len; } Emit;
#define EMITS_CAP 8
static Emit   g_emits[EMITS_CAP];
static size_t g_emit_n;
static void recorder(const uint8_t *p, size_t n, void *user) {
    (void)user;
    if (g_emit_n >= EMITS_CAP) return;
    Emit *e = &g_emits[g_emit_n++];
    if (n > sizeof e->bytes) n = sizeof e->bytes;
    memcpy(e->bytes, p, n);
    e->len = n;
}
static void reset_emits(void) { g_emit_n = 0; memset(g_emits, 0, sizeof g_emits); }

/* Build datagram for (nal_id, chunk_index ci) of a NAL of total_len. Payload
 * bytes are a deterministic function of (nal_id, absolute offset). Returns len. */
static size_t mkchunk(uint8_t *out, uint16_t nal_id, uint16_t total_len, size_t ci) {
    size_t off = ci * LANFRAG_CHUNK_PAYLOAD;
    size_t take = total_len - off;
    if (take > LANFRAG_CHUNK_PAYLOAD) take = LANFRAG_CHUNK_PAYLOAD;
    lanfrag_pack_hdr(out, nal_id, (uint16_t)off, total_len);
    for (size_t i = 0; i < take; i++)
        out[LANFRAG_HDR_BYTES + i] = (uint8_t)((nal_id + off + i) & 0xFF);
    return LANFRAG_HDR_BYTES + take;
}

/* Expected payload byte at absolute offset for a given nal_id. */
static uint8_t expect_byte(uint16_t nal_id, size_t off) {
    return (uint8_t)((nal_id + off) & 0xFF);
}

static void test_in_order_complete(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 3001;   /* 3 chunks: 1400,1400,201 */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    for (size_t ci = 0; ci < 3; ci++) {
        size_t n = mkchunk(dg, 5, total, ci);
        lan_reasm_feed(&r, dg, n, 1000 + ci);
    }
    EXPECT(g_emit_n == 1, "one emit");
    EXPECT(g_emits[0].len == total, "full len");
    EXPECT(r.nals_complete == 1 && r.nals_partial == 0, "complete count");
    EXPECT(g_emits[0].bytes[0] == expect_byte(5, 0), "byte0");
    EXPECT(g_emits[0].bytes[2000] == expect_byte(5, 2000), "byte2000");
    EXPECT(g_emits[0].bytes[3000] == expect_byte(5, 3000), "byte3000");
}

static void test_out_of_order(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 3001;
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    size_t order[3] = {2, 0, 1};
    for (size_t k = 0; k < 3; k++) {
        size_t n = mkchunk(dg, 6, total, order[k]);
        lan_reasm_feed(&r, dg, n, 1000 + k);
    }
    EXPECT(g_emit_n == 1, "one emit ooo");
    EXPECT(g_emits[0].len == total, "full len ooo");
    EXPECT(r.nals_complete == 1, "complete ooo");
    EXPECT(g_emits[0].bytes[1500] == expect_byte(6, 1500), "byte1500 ooo");
}

static void test_tail_loss_via_supersede(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 4200;   /* 3 full chunks */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    /* NAL 10: chunks 0,1 arrive; chunk 2 (tail) lost. */
    lan_reasm_feed(&r, dg, mkchunk(dg, 10, total, 0), 100);
    lan_reasm_feed(&r, dg, mkchunk(dg, 10, total, 1), 101);
    /* NAL 11 starts -> not enough to evict yet (slots free). Force flush via tick. */
    lan_reasm_feed(&r, dg, mkchunk(dg, 11, total, 0), 102);
    /* still open; advance past (last_touch 102 + timeout) and tick to flush
     * NAL10 then NAL11(partial). */
    lan_reasm_tick(&r, 200 + LAN_REASM_TIMEOUT_NS);
    EXPECT(g_emit_n >= 1, "emitted on timeout");
    EXPECT(g_emits[0].len == 2 * LANFRAG_CHUNK_PAYLOAD, "tail-loss prefix = 2800");
    EXPECT(r.nals_partial >= 1, "partial counted");
    EXPECT(g_emits[0].bytes[2799] == expect_byte(10, 2799), "prefix byte");
}

static void test_mid_hole_truncates(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 4200;   /* chunks 0,1,2 */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    /* chunk 0 and 2 arrive, chunk 1 (middle) lost */
    lan_reasm_feed(&r, dg, mkchunk(dg, 20, total, 0), 100);
    lan_reasm_feed(&r, dg, mkchunk(dg, 20, total, 2), 101);
    lan_reasm_tick(&r, 200 + LAN_REASM_TIMEOUT_NS);  /* past last_touch 101 + timeout */
    EXPECT(g_emit_n == 1, "one emit mid-hole");
    EXPECT(g_emits[0].len == LANFRAG_CHUNK_PAYLOAD, "prefix = 1400 (up to hole)");
    EXPECT(r.nals_partial == 1, "partial mid-hole");
}

static void test_first_chunk_lost_emits_nothing(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 4200;
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    /* chunks 1,2 arrive, chunk 0 lost -> no contiguous prefix */
    lan_reasm_feed(&r, dg, mkchunk(dg, 30, total, 1), 100);
    lan_reasm_feed(&r, dg, mkchunk(dg, 30, total, 2), 101);
    lan_reasm_tick(&r, 200 + LAN_REASM_TIMEOUT_NS);  /* past last_touch 101 + timeout */
    EXPECT(g_emit_n == 0, "no bytes emitted");
    EXPECT(r.nals_partial == 1, "still counted partial");
}

static void test_supersede_evicts_when_full(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 2800;   /* 2 chunks each */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    /* Open 5 NALs each with only chunk 0 -> the 5th forces eviction of NAL 40. */
    for (uint16_t id = 40; id <= 44; id++)
        lan_reasm_feed(&r, dg, mkchunk(dg, id, total, 0), 100 + id);
    /* NAL 40 evicted as partial (prefix=1400), ascending order. */
    EXPECT(g_emit_n == 1, "one eviction");
    EXPECT(g_emits[0].len == LANFRAG_CHUNK_PAYLOAD, "evicted prefix 1400");
    EXPECT(r.nals_partial == 1, "evicted partial");
    EXPECT(r.last_emitted == 40, "evicted oldest first");
}

static void test_stale_chunk_dropped(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 1400;   /* single chunk NAL */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    lan_reasm_feed(&r, dg, mkchunk(dg, 50, total, 0), 100);  /* completes + emits */
    EXPECT(r.nals_complete == 1, "50 complete");
    /* a late duplicate of NAL 50 after it was emitted -> dropped */
    lan_reasm_feed(&r, dg, mkchunk(dg, 50, total, 0), 101);
    EXPECT(r.chunks_dropped == 1, "stale dropped");
    EXPECT(g_emit_n == 1, "no second emit");
}

static void test_bad_and_invalid(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint8_t dg[LANFRAG_HDR_BYTES + 16];
    /* bad magic (single 16-byte chunk fits dg, then corrupt the magic) */
    size_t n = mkchunk(dg, 1, 16, 0); dg[0] = 0x00;
    lan_reasm_feed(&r, dg, n, 1);
    EXPECT(r.bad_magic == 1, "bad magic counted");
    /* total_len 0 */
    lanfrag_pack_hdr(dg, 1, 0, 0);
    lan_reasm_feed(&r, dg, LANFRAG_HDR_BYTES + 4, 2);
    EXPECT(r.nal_degenerate == 1, "total0 counted");
    /* misaligned frag_off */
    lanfrag_pack_hdr(dg, 1, 5 /* not a multiple of 1400 */, 3000);
    lan_reasm_feed(&r, dg, LANFRAG_HDR_BYTES + 4, 3);
    EXPECT(r.chunks_dropped == 1, "misaligned dropped");
}

static void test_inflight_duplicate_ignored(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 2800;   /* 2 chunks */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    /* chunk 0 arrives (slot open), then a DUPLICATE of chunk 0 with corrupted
     * payload, then chunk 1. The first write must win (dedup ignores the dup);
     * no double-count, no corruption. */
    lan_reasm_feed(&r, dg, mkchunk(dg, 60, total, 0), 100);
    size_t ndup = mkchunk(dg, 60, total, 0);
    dg[LANFRAG_HDR_BYTES] ^= 0xFF;   /* corrupt the dup's first payload byte */
    lan_reasm_feed(&r, dg, ndup, 101);
    lan_reasm_feed(&r, dg, mkchunk(dg, 60, total, 1), 102);
    EXPECT(g_emit_n == 1, "one emit after dup");
    EXPECT(g_emits[0].len == total, "full len after dup");
    EXPECT(r.nals_complete == 1, "complete despite dup");
    EXPECT(r.chunks_dropped == 0, "dup is a silent no-op, not a drop");
    EXPECT(g_emits[0].bytes[0] == expect_byte(60, 0), "first chunk0 write wins");
}

static void test_full_max_nal(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = LANFRAG_MAX_NAL;   /* 65535 -> 47 chunks (ci 0..46) */
    size_t nc = (total + LANFRAG_CHUNK_PAYLOAD - 1) / LANFRAG_CHUNK_PAYLOAD;
    EXPECT(nc == 47, "47 chunks at max");
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    for (size_t ci = 0; ci < nc; ci++)
        lan_reasm_feed(&r, dg, mkchunk(dg, 77, total, ci), 100 + ci);
    EXPECT(g_emit_n == 1, "one emit at max");
    EXPECT(g_emits[0].len == total, "full 65535 len");
    EXPECT(r.nals_complete == 1, "complete at max");
    EXPECT(g_emits[0].bytes[65000] == expect_byte(77, 65000), "high-offset byte");
    EXPECT(g_emits[0].bytes[total - 1] == expect_byte(77, total - 1), "last byte");
}

static void test_caster_restart_recovers(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 1400;   /* single-chunk NALs */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    /* Player has been running a while: emit a high nal_id. */
    lan_reasm_feed(&r, dg, mkchunk(dg, 5000, total, 0), 100);
    EXPECT(r.nals_complete == 1 && r.last_emitted == 5000, "high id emitted");
    /* Caster restarts -> nal_id resets to 0. The fresh low-id stream is a far
     * backward jump (not a stale straggler) and MUST be accepted immediately --
     * regression for the multi-minute blackout where the whole 32768-id past was
     * treated as stale. */
    lan_reasm_feed(&r, dg, mkchunk(dg, 0, total, 0), 200);
    lan_reasm_feed(&r, dg, mkchunk(dg, 1, total, 0), 201);
    EXPECT(r.nals_complete == 3, "restart stream accepted immediately");
    EXPECT(r.chunks_dropped == 0, "no stale-drop on restart");
}

static void test_stale_window_boundary(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 1400;
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    lan_reasm_feed(&r, dg, mkchunk(dg, 100, total, 0), 1);   /* last_emitted=100 */
    EXPECT(r.nals_complete == 1, "base emitted");
    /* just inside the window (back = WINDOW-1): dropped as stale */
    lan_reasm_feed(&r, dg, mkchunk(dg, (uint16_t)(100 - (LAN_REASM_STALE_WINDOW - 1)),
                                   total, 0), 2);
    EXPECT(r.chunks_dropped == 1, "within-window straggler dropped");
    EXPECT(r.nals_complete == 1, "no emit for stale");
    /* exactly at the window edge (back = WINDOW): treated as fresh, accepted */
    lan_reasm_feed(&r, dg, mkchunk(dg, (uint16_t)(100 - LAN_REASM_STALE_WINDOW),
                                   total, 0), 3);
    EXPECT(r.nals_complete == 2, "window-edge accepted as fresh");
}

static void test_nal_id_wrap(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 1400;   /* single chunk */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    lan_reasm_feed(&r, dg, mkchunk(dg, 65535, total, 0), 100);
    lan_reasm_feed(&r, dg, mkchunk(dg, 0,     total, 0), 101);   /* wrap, newer */
    EXPECT(r.nals_complete == 2, "both complete across wrap");
    EXPECT(r.chunks_dropped == 0, "wrap not mistaken for stale");
    EXPECT(r.last_emitted == 0, "last emitted is the wrapped id");
}

/* nal_gaps: whole single-fragment NALs missing from the caster id sequence are
 * invisible to nals_partial (no slot opens) but must be counted as a gap. */
static void test_nal_gaps_whole_nal_loss(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 100;   /* single chunk -> completes + flushes immediately */
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    uint16_t ids[3] = { 10, 11, 14 };   /* 12 and 13 lost entirely */
    for (size_t k = 0; k < 3; k++) {
        size_t n = mkchunk(dg, ids[k], total, 0);
        lan_reasm_feed(&r, dg, n, 1000 + k);
    }
    EXPECT(r.nals_complete == 3, "gaps: 3 complete");
    EXPECT(r.nals_partial == 0, "gaps: no partials (loss is invisible to it)");
    EXPECT(r.nal_gaps == 2, "gaps: 2 whole NALs missing (12,13)");
}

/* A contiguous run has zero gaps; a large backward jump (caster restart) must
 * not inflate the gap count. */
static void test_nal_gaps_none_on_contiguous_and_restart(void) {
    reset_emits();
    LanReasm r; lan_reasm_init(&r, recorder, NULL);
    uint16_t total = 100;
    uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD];
    uint16_t ids[4] = { 40000, 40001, 40002, 3 };   /* 3 = restart rebase */
    for (size_t k = 0; k < 4; k++) {
        size_t n = mkchunk(dg, ids[k], total, 0);
        lan_reasm_feed(&r, dg, n, 1000 + k);
    }
    EXPECT(r.nal_gaps == 0, "gaps: contiguous + restart => 0 gaps");
}

int main(void) {
    test_in_order_complete();
    test_out_of_order();
    test_tail_loss_via_supersede();
    test_mid_hole_truncates();
    test_first_chunk_lost_emits_nothing();
    test_supersede_evicts_when_full();
    test_stale_chunk_dropped();
    test_bad_and_invalid();
    test_inflight_duplicate_ignored();
    test_full_max_nal();
    test_caster_restart_recovers();
    test_stale_window_boundary();
    test_nal_id_wrap();
    test_nal_gaps_whole_nal_loss();
    test_nal_gaps_none_on_contiguous_and_restart();
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all lan_reasm tests passed\n");
    return 0;
}
