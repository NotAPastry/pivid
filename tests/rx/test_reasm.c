/* Host-side tests for apps/pivid-rx/src/reasm.c.
 *
 * Each test_* function exercises one slice of the reassembler state machine.
 * The emit callback writes into a recording buffer so we can assert on
 * payload bytes as well as counters. */

#define _POSIX_C_SOURCE 200809L

#include "../apps/pivid-rx/src/reasm.h"
#include "protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* --- Emit recorder -------------------------------------------------- */

typedef struct {
    uint8_t bytes[REASM_BUF_BYTES];
    size_t  len;
} Emit_Record;

#define EMITS_CAP 8
static Emit_Record g_emits[EMITS_CAP];
static size_t      g_emit_count;

static void recorder(const uint8_t *p, size_t n, void *user) {
    (void)user;
    if (g_emit_count >= EMITS_CAP) return;
    Emit_Record *e = &g_emits[g_emit_count++];
    if (n > sizeof e->bytes) n = sizeof e->bytes;
    memcpy(e->bytes, p, n);
    e->len = n;
}

static void reset_recorder(void) {
    memset(g_emits, 0, sizeof g_emits);
    g_emit_count = 0;
}

/* --- Packet builder -------------------------------------------------- */

/* Build a FIXED-mode [flags][len u16-LE][payload] record (3-byte header).
 * reasm_feed parses with PROTO_MODE_FIXED, so the header is 3 bytes here.
 * Returns total bytes written. */
static size_t build_record(uint8_t *out, int start, int end, uint8_t seq,
                           const uint8_t *payload, size_t payload_len) {
    uint8_t flags = PROTO_FLAG_SEQ(seq);
    if (start) flags |= PROTO_FLAG_START;
    if (end)   flags |= PROTO_FLAG_END;
    out[0] = flags;
    pack_le16(&out[1], (uint16_t)payload_len);
    if (payload_len) memcpy(&out[3], payload, payload_len);
    return 3 + payload_len;
}

static void fill_pattern(uint8_t *buf, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(seed + i);
}

/* Build the START record of a MULTI-fragment train. Its payload is
 * [total_len u16-LE][first chunk]; total_len is the full NAL byte length.
 * reasm strips the 2-byte prefix on the multi-fragment START path. */
static size_t build_start_record(uint8_t *out, uint8_t seq, uint16_t total_len,
                                 const uint8_t *chunk, size_t chunk_len) {
    uint8_t pl[2 + PROTOCOL_MAX_PAYLOAD];
    pack_le16(pl, total_len);
    memcpy(pl + 2, chunk, chunk_len);
    return build_record(out, /*start=*/1, /*end=*/0, seq, pl, 2 + chunk_len);
}

/* --- Case 1: single-packet NAL fast path ----------------------------- */
static void test_single_packet_nal(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    uint8_t payload[50]; fill_pattern(payload, sizeof payload, 0x10);
    uint8_t rec[3 + 50];
    size_t rec_len = build_record(rec, 1, 1, 0, payload, sizeof payload);
    reasm_feed(&r, rec, rec_len);

    EXPECT(g_emit_count == 1, "single-packet: one emit");
    EXPECT(r.nals_emitted == 1, "single-packet: nals_emitted == 1");
    EXPECT(g_emits[0].len == 50, "single-packet: payload length matches");
    EXPECT(memcmp(g_emits[0].bytes, payload, 50) == 0, "single-packet: payload bytes match");
    EXPECT(r.in_progress == 0, "single-packet: no leftover state");
}

/* --- Case 2: 8-byte single-packet NAL (regression: must NOT be dropped) */
static void test_eight_byte_single_packet_nal(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    /* An 8-byte payload happens to be the same length as a bind packet on
     * the wire. The reassembler must treat it as a NAL regardless --
     * bind/NAL discrimination happens at the radio sync-word layer, not
     * here. If a future change re-introduces a length==8 special case,
     * this test catches it. */
    uint8_t payload[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    uint8_t rec[3 + 8];
    size_t rec_len = build_record(rec, 1, 1, 0, payload, sizeof payload);
    reasm_feed(&r, rec, rec_len);

    EXPECT(g_emit_count == 1, "8-byte: emitted as NAL");
    EXPECT(r.nals_emitted == 1, "8-byte: nals_emitted == 1");
    EXPECT(g_emits[0].len == 8, "8-byte: length 8 preserved");
    EXPECT(memcmp(g_emits[0].bytes, payload, 8) == 0, "8-byte: payload preserved");
}

/* --- Case 3: three-packet train -------------------------------------- */
static void test_three_packet_train(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    uint8_t p0[100], p1[100], p2[53];
    fill_pattern(p0, sizeof p0, 0x10);
    fill_pattern(p1, sizeof p1, 0x80);
    fill_pattern(p2, sizeof p2, 0xC0);

    uint8_t rec[3 + PROTOCOL_MAX_PAYLOAD];
    size_t n;

    n = build_start_record(rec, 5, 253, p0, sizeof p0); reasm_feed(&r, rec, n);
    EXPECT(r.in_progress == 1, "train: in_progress after START");

    n = build_record(rec, 0, 0, 6, p1, sizeof p1); reasm_feed(&r, rec, n);
    n = build_record(rec, 0, 1, 7, p2, sizeof p2); reasm_feed(&r, rec, n);

    EXPECT(g_emit_count == 1, "train: one emit");
    EXPECT(r.nals_emitted == 1, "train: nals_emitted == 1");
    EXPECT(g_emits[0].len == 253, "train: concatenated length 253");
    EXPECT(memcmp(g_emits[0].bytes,        p0, 100) == 0, "train: first chunk bytes");
    EXPECT(memcmp(g_emits[0].bytes + 100,  p1, 100) == 0, "train: second chunk bytes");
    EXPECT(memcmp(g_emits[0].bytes + 200,  p2,  53) == 0, "train: third chunk bytes");
    EXPECT(r.in_progress == 0, "train: cleared after END");
}

/* --- Case 4: SEQ mismatch in middle ---------------------------------- */
static void test_seq_mismatch(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    uint8_t p[40]; fill_pattern(p, sizeof p, 0x20);
    uint8_t rec[3 + 2 + 40];
    size_t n;

    n = build_start_record(rec, 3, 40, p, sizeof p); reasm_feed(&r, rec, n);
    /* Skip SEQ 4; feed SEQ 5 -- should discard partial. */
    n = build_record(rec, 0, 0, 5, p, sizeof p); reasm_feed(&r, rec, n);

    EXPECT(g_emit_count == 0, "seq-mismatch: no emit");
    EXPECT(r.nals_emitted == 0, "seq-mismatch: nals_emitted == 0");
    EXPECT(r.dropped_seq_mismatch == 1, "seq-mismatch: counter bumped");
    EXPECT(r.in_progress == 0, "seq-mismatch: partial discarded");

    /* A fresh START should now start a new reassembly cleanly. */
    n = build_record(rec, 1, 1, 0, p, sizeof p); reasm_feed(&r, rec, n);
    EXPECT(g_emit_count == 1, "seq-mismatch: subsequent START emits");
    EXPECT(r.nals_emitted == 1, "seq-mismatch: nals_emitted == 1 after restart");
}

/* --- Case 5: SEQ wraps the 4-bit counter ----------------------------- */
static void test_seq_wrap(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    uint8_t p[20]; fill_pattern(p, sizeof p, 0x40);
    uint8_t rec[3 + 2 + 20];
    size_t n;

    /* SEQs 14, 15, 0, 1 -- each step is (prev+1) & 0xF. */
    n = build_start_record(rec, 14, 80, p, sizeof p); reasm_feed(&r, rec, n);
    n = build_record(rec, 0, 0, 15, p, sizeof p); reasm_feed(&r, rec, n);
    n = build_record(rec, 0, 0,  0, p, sizeof p); reasm_feed(&r, rec, n);
    n = build_record(rec, 0, 1,  1, p, sizeof p); reasm_feed(&r, rec, n);

    EXPECT(g_emit_count == 1, "wrap: one emit");
    EXPECT(r.dropped_seq_mismatch == 0, "wrap: no seq mismatches");
    EXPECT(g_emits[0].len == 80, "wrap: concatenated length 80");
}

/* --- Case 6: overflow ------------------------------------------------ */
static void test_overflow(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    /* Feed enough max-size (253 B) packets to exceed REASM_BUF_BYTES.
     * 65536 / 253 = ~259, so ceil = 260 packets. Use 260 (START + 259
     * continuations) to guarantee tripping the overflow check on a
     * continuation push. SEQ walks 0..15 and wraps multiple times --
     * valid SEQ-wise because consecutive packets always have
     * seq == (expected_seq) mod 16. */
    const int total = (REASM_BUF_BYTES / PROTOCOL_MAX_PAYLOAD) + 1;
    uint8_t p[PROTOCOL_MAX_PAYLOAD];
    fill_pattern(p, sizeof p, 0x55);
    uint8_t rec[3 + PROTOCOL_MAX_PAYLOAD];
    size_t n;

    /* The multi-fragment START carries a 2-byte total-length prefix, so its
     * chunk is capped at PROTOCOL_MAX_PAYLOAD - 2 to keep the inner payload
     * within the protocol ceiling; continuations stay full size. The
     * advertised total (0) is irrelevant -- overflow trips before END. */
    n = build_start_record(rec, 0, 0, p, sizeof p - 2); reasm_feed(&r, rec, n);
    for (int i = 1; i < total; i++) {
        uint8_t seq = (uint8_t)(i & 0x0F);
        int is_end = 0;  /* never end; we expect overflow before END */
        n = build_record(rec, 0, is_end, seq, p, sizeof p);
        reasm_feed(&r, rec, n);
    }

    EXPECT(r.dropped_overflow == 1, "overflow: counter bumped");
    EXPECT(r.in_progress == 0, "overflow: partial discarded");
    EXPECT(g_emit_count == 0, "overflow: no emit");
}

/* --- Case 7: timeout via reasm_tick ---------------------------------- */
static void test_timeout(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    uint8_t p[30]; fill_pattern(p, sizeof p, 0x60);
    uint8_t rec[3 + 2 + 30];
    size_t n = build_start_record(rec, 0, 30, p, sizeof p);
    reasm_feed(&r, rec, n);
    EXPECT(r.in_progress == 1, "timeout: in_progress after START");

    /* Poke the last-progress timestamp 600 ms into the past. */
    r.last_progress_ns -= 600000000ull;
    reasm_tick(&r);

    EXPECT(r.dropped_timeout == 1, "timeout: counter bumped");
    EXPECT(r.in_progress == 0, "timeout: partial discarded");
    EXPECT(g_emit_count == 0, "timeout: no emit");
}

/* --- Case 8: START arrives while in-progress ------------------------- */
static void test_start_mid_train(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    uint8_t p[40]; fill_pattern(p, sizeof p, 0x70);
    uint8_t rec[3 + 2 + 40];
    size_t n;

    n = build_start_record(rec, 0, 40, p, sizeof p); reasm_feed(&r, rec, n);
    /* Second START with no END in between: discard the previous, start fresh. */
    n = build_start_record(rec, 0, 80, p, sizeof p); reasm_feed(&r, rec, n);

    EXPECT(r.dropped_seq_mismatch == 1, "start-mid: counter bumped");
    EXPECT(r.in_progress == 1, "start-mid: in_progress for the new train");
    EXPECT(r.buf_len == 40, "start-mid: buffer reset to new payload only");
    EXPECT(g_emit_count == 0, "start-mid: no emit yet");

    /* Now close the second train and confirm it emits cleanly. */
    n = build_record(rec, 0, 1, 1, p, sizeof p); reasm_feed(&r, rec, n);
    EXPECT(g_emit_count == 1, "start-mid: second train emits");
    EXPECT(g_emits[0].len == 80, "start-mid: emit is the second train, not first");
}

/* --- Case 9: invalid packet ----------------------------------------- */
static void test_invalid_packet(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    /* Get into in-progress state first. */
    uint8_t p[30]; fill_pattern(p, sizeof p, 0x80);
    uint8_t rec[3 + 2 + 30];
    size_t n = build_start_record(rec, 0, 30, p, sizeof p);
    reasm_feed(&r, rec, n);
    EXPECT(r.in_progress == 1, "invalid: in_progress after START");

    size_t buf_len_before = r.buf_len;
    uint8_t expected_seq_before = r.expected_seq;

    /* Garbage record: FIXED u16-LE len field (bytes[1..2]) claims a huge
     * length, but only 4 bytes total supplied -- protocol_parse should
     * reject (over-range AND under-bound). */
    uint8_t bad[4] = { 0x00, 0xC8, 0xDE, 0xAD };  /* len = 0xDEC8 */
    reasm_feed(&r, bad, sizeof bad);

    EXPECT(r.dropped_invalid == 1, "invalid: counter bumped");
    EXPECT(r.in_progress == 1, "invalid: in-progress unchanged");
    EXPECT(r.buf_len == buf_len_before, "invalid: buffer length unchanged");
    EXPECT(r.expected_seq == expected_seq_before, "invalid: expected_seq unchanged");
    EXPECT(g_emit_count == 0, "invalid: no emit");
}

/* Duplicate-SEQ in a continuation: after START+SEQ=0, a second packet at
 * SEQ=0 (instead of SEQ=1) is a continuity violation -- treated as a fresh
 * START semantics-wise. The reassembler bumps dropped_seq_mismatch and
 * discards the partial NAL. */
static void test_duplicate_seq_in_continuation(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    uint8_t p1[30]; for (int i = 0; i < 30; i++) p1[i] = (uint8_t)i;
    uint8_t p2[30]; for (int i = 0; i < 30; i++) p2[i] = (uint8_t)(0x80 + i);

    uint8_t rec[3 + 2 + 30];

    /* START, SEQ=0. */
    size_t n = build_start_record(rec, 0, 30, p1, sizeof p1);
    reasm_feed(&r, rec, n);
    EXPECT(r.in_progress == 1, "dup-seq: in-progress after START");

    /* Now a SEQ=0 continuation (should be SEQ=1). Either path -- treated
     * as a fresh train or as a mismatch -- must NOT silently accept the
     * duplicate as continuation. */
    n = build_record(rec, 0, 0, 0, p2, sizeof p2);
    reasm_feed(&r, rec, n);
    EXPECT(r.dropped_seq_mismatch >= 1, "dup-seq: dropped_seq_mismatch bumped");
    EXPECT(g_emit_count == 0, "dup-seq: no NAL emitted");
}

/* Empty payload (length=0) single-packet NAL. protocol_parse accepts it;
 * the reassembler should emit a zero-length NAL via the recorder. */
static void test_zero_payload_single_packet(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    uint8_t rec[3];
    size_t n = build_record(rec, 1, 1, 0, NULL, 0);
    EXPECT(n == 3, "zero-payload: record is 3 bytes (flags + u16 length)");
    reasm_feed(&r, rec, n);
    EXPECT(g_emit_count == 1, "zero-payload: one emit");
    EXPECT(g_emits[0].len == 0, "zero-payload: emitted len is 0");
    EXPECT(r.nals_emitted == 1, "zero-payload: nals_emitted == 1");
}

/* --- Case 12: 16-fragment-multiple loss must NOT splice (length guard). --- */
static void test_seq_alias_16_drop(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    enum { NFRAG = 40, CHUNK = 100 };
    const uint16_t total = NFRAG * CHUNK;   /* 4000 */
    uint8_t chunk[CHUNK];
    uint8_t rec[3 + 2 + CHUNK];
    size_t n;

    /* START (seq 0) carries the total-length prefix. */
    fill_pattern(chunk, CHUNK, 0x00);
    n = build_start_record(rec, 0, total, chunk, CHUNK);
    reasm_feed(&r, rec, n);

    /* Continuations 1..3 (seq 1..3). expected_seq becomes 4. */
    for (int i = 1; i <= 3; i++) {
        fill_pattern(chunk, CHUNK, (uint8_t)(i * 7));
        n = build_record(rec, 0, 0, (uint8_t)(i & 0x0F), chunk, CHUNK);
        reasm_feed(&r, rec, n);
    }
    /* DROP fragments 4..19 (exactly 16). The next survivor is index 20 whose
     * seq (20 & 0xF) == 4 == expected_seq -> it ALIASES and would be spliced
     * without the length guard. Feed 20..39, last is END. */
    for (int i = 20; i < NFRAG; i++) {
        int end = (i == NFRAG - 1);
        fill_pattern(chunk, CHUNK, (uint8_t)(i * 7));
        n = build_record(rec, 0, end, (uint8_t)(i & 0x0F), chunk, CHUNK);
        reasm_feed(&r, rec, n);
    }

    EXPECT(g_emit_count == 0, "alias-16: corrupt NAL NOT emitted");
    EXPECT(r.nals_emitted == 0, "alias-16: nals_emitted stays 0");
    EXPECT(r.dropped_len_mismatch == 1, "alias-16: length-mismatch counter bumped");
    EXPECT(r.in_progress == 0, "alias-16: partial cleared");
}

/* --- Case 13: clean multi-fragment train with prefix reassembles intact. --- */
static void test_multifrag_len_ok(void) {
    reset_recorder();
    Reasm r; reasm_init(&r, recorder, NULL);

    enum { CHUNK = 100 };
    uint8_t c0[CHUNK], c1[CHUNK], c2[53];
    fill_pattern(c0, CHUNK, 0x10);
    fill_pattern(c1, CHUNK, 0x80);
    fill_pattern(c2, 53, 0xC0);
    const uint16_t total = CHUNK + CHUNK + 53;   /* 253 */
    uint8_t rec[3 + 2 + CHUNK];
    size_t n;

    n = build_start_record(rec, 5, total, c0, CHUNK); reasm_feed(&r, rec, n);
    n = build_record(rec, 0, 0, 6, c1, CHUNK);        reasm_feed(&r, rec, n);
    n = build_record(rec, 0, 1, 7, c2, 53);           reasm_feed(&r, rec, n);

    EXPECT(g_emit_count == 1, "multifrag-ok: emits once");
    EXPECT(r.dropped_len_mismatch == 0, "multifrag-ok: no false mismatch");
    EXPECT(g_emits[0].len == 253, "multifrag-ok: full length");
    EXPECT(memcmp(g_emits[0].bytes,           c0, CHUNK) == 0, "multifrag-ok: chunk0");
    EXPECT(memcmp(g_emits[0].bytes + CHUNK,    c1, CHUNK) == 0, "multifrag-ok: chunk1");
    EXPECT(memcmp(g_emits[0].bytes + 2*CHUNK,  c2, 53)    == 0, "multifrag-ok: chunk2");
}

/* --- Driver ---------------------------------------------------------- */

int main(void) {
    test_single_packet_nal();
    test_eight_byte_single_packet_nal();
    test_three_packet_train();
    test_seq_mismatch();
    test_seq_wrap();
    test_overflow();
    test_timeout();
    test_start_mid_train();
    test_invalid_packet();
    test_duplicate_seq_in_continuation();
    test_zero_payload_single_packet();
    test_seq_alias_16_drop();
    test_multifrag_len_ok();

    if (g_failures) {
        fprintf(stderr, "test_reasm: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "test_reasm: all 13 cases passed\n");
    return 0;
}
