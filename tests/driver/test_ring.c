#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/lr2021/lr2021_ring.h"

/* These tests exercise the pure helpers from lr2021_ring.h on host hardware.
 * They simulate the kernel-side push/pop discipline without using any kernel
 * primitives -- single-threaded, no memory barriers. The pure helpers contain
 * all the wraparound and arithmetic logic; the kernel wrappers only add
 * smp_store_release / smp_load_acquire on the index updates. */

static void test_static_helpers_basic(void) {
    /* used: simple case. */
    assert(lr2021_ring_used(100, 50) == 50);
    /* used: same indices => empty. */
    assert(lr2021_ring_used(7, 7) == 0);

    /* free of empty ring. */
    assert(lr2021_ring_free(0, 0) == RADIO_RING_DATA_BYTES - 1);

    /* offset: edge cases. */
    assert(lr2021_ring_offset(0) == 0);
    assert(lr2021_ring_offset(RADIO_RING_DATA_BYTES - 1) == RADIO_RING_DATA_BYTES - 1);
    assert(lr2021_ring_offset(RADIO_RING_DATA_BYTES) == 0);            /* wraps cleanly */
    assert(lr2021_ring_offset(RADIO_RING_DATA_BYTES + 5) == 5);
    /* uint32 wrap: at 0xFFFFFFFF the masked offset is (DATA_MASK). */
    assert(lr2021_ring_offset(0xFFFFFFFFu) == RADIO_RING_DATA_MASK);
    /* And 2^32 (which wraps to 0) gives offset 0. */
    assert(lr2021_ring_offset(0u) == 0);
}

static void test_empty_pop_returns_zero(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    __u8 out[256];
    __u16 out_len = 0xFFFF;
    __u32 rd = r->rd_idx;
    int rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 0);
    assert(rd == 0);              /* not advanced */
    assert(out_len == 0xFFFF);    /* not written */

    free(r);
}

static void test_push_pop_round_trip(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    const __u8 payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    const __u8 plen = (__u8)sizeof(payload);

    /* Push. */
    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, plen);
    assert(rc == 0);
    assert(wr == (__u32)(2u + plen));  /* 2 length bytes + payload bytes */
    r->wr_idx = wr;

    /* Pop. */
    __u8 out[256] = {0};
    __u16 out_len = 0;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == plen);
    assert(memcmp(out, payload, plen) == 0);
    r->rd_idx = rd;

    /* Ring is empty again. */
    assert(r->wr_idx == r->rd_idx);

    free(r);
}

static void test_push_pop_many_records_fifo(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    /* Push 50 records of varying length. */
    __u32 wr = 0;
    for (int i = 0; i < 50; i++) {
        __u8 payload[64];
        __u8 len = (__u8)((i % 64) + 1);   /* 1..64 */
        for (int j = 0; j < len; j++) payload[j] = (__u8)(i * 13 + j);
        int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, len);
        assert(rc == 0);
    }
    r->wr_idx = wr;

    /* Pop and check FIFO order. */
    __u32 rd = 0;
    for (int i = 0; i < 50; i++) {
        __u8 out[64];
        __u16 out_len = 0;
        int rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
        assert(rc == 1);
        __u8 expected_len = (__u8)((i % 64) + 1);
        assert(out_len == expected_len);
        for (int j = 0; j < expected_len; j++) {
            assert(out[j] == (__u8)(i * 13 + j));
        }
    }
    r->rd_idx = rd;

    /* Empty after. */
    __u8 junk_out[1];
    __u16 junk_len;
    __u32 rd2 = r->rd_idx;
    assert(lr2021_ring_pop_pure(r->wr_idx, &rd2, r->data, junk_out, &junk_len) == 0);

    free(r);
}

static void test_full_ring_rejects_push(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    /* Push max-size records (256 bytes per (2 len + 254 payload)) until full. */
    __u8 payload[254];
    memset(payload, 0xAA, sizeof(payload));

    __u32 wr = 0;
    int pushed = 0;
    while (lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, 254) == 0) {
        pushed++;
        if (pushed > 1000) { assert(0 && "ring never filled"); }
    }

    /* Free space at refusal must be strictly less than what a max record needs. */
    assert(lr2021_ring_free(wr, r->rd_idx) < (__u32)(2u + 254u));
    /* And we should have pushed at least ~100 records (32767/255 ~= 128). */
    assert(pushed >= 100);

    free(r);
}

static void test_record_wraps_data_boundary(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    /* Synthetic: set both indices near the end of data[] so the next 20-byte
     * record straddles the end-of-data[] boundary (wraps via mask). */
    __u32 base = RADIO_RING_DATA_BYTES - 10;
    r->wr_idx = base;
    r->rd_idx = base;                 /* still empty since wr == rd */

    __u8 payload[20];
    for (int j = 0; j < 20; j++) payload[j] = (__u8)(0x80 + j);

    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, 20);
    assert(rc == 0);
    r->wr_idx = wr;

    /* Verify the bytes actually landed in the right physical positions. */
    /* Low prefix byte (20) at DATA-10, high prefix byte (0) at DATA-9. */
    assert(r->data[RADIO_RING_DATA_BYTES - 10] == 20);
    assert(r->data[RADIO_RING_DATA_BYTES - 9]  == 0);
    /* Payload now starts at DATA-8: first 8 bytes there, remainder wraps. */
    for (int j = 0; j < 8; j++)
        assert(r->data[RADIO_RING_DATA_BYTES - 8 + j] == (__u8)(0x80 + j));
    for (int j = 0; j < 12; j++)
        assert(r->data[j] == (__u8)(0x80 + 8 + j));

    /* Pop and verify the record came out intact. */
    __u8 out[20] = {0};
    __u16 out_len = 0;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == 20);
    assert(memcmp(out, payload, 20) == 0);

    free(r);
}

static void test_uint32_overflow_wraparound(void) {
    /* The whole point of insisting on a power-of-two data buffer: physical
     * offsets must stay continuous across the uint32 index overflow. */
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    /* 16 less than 2^32, so the next push (2 + 20 = 22 bytes) overflows. */
    __u32 base = 0xFFFFFFF0u;
    r->wr_idx = base;
    r->rd_idx = base;

    __u8 payload[20];
    for (int j = 0; j < 20; j++) payload[j] = (__u8)(0xC0 + j);

    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, 20);
    assert(rc == 0);
    /* wr wrapped past 2^32 (initial 0xFFFFFFF0 + 22 = 0x00000006). */
    assert(wr == 6u);
    assert(wr < base);            /* sanity: numerically smaller than the start */
    r->wr_idx = wr;

    /* Pop and verify. The physical offsets:
     *   low length byte at offset(0xFFFFFFF0) = 0xFFFFFFF0 & MASK
     *                                    = 0xFFFFFFF0 & 0x7FFF
     *                                    = 0x7FF0       (== DATA_BYTES - 16)
     *   high length byte at 0x7FF1, payload starts at 0x7FF2, wraps at
     *   0x8000 -> 0, continues to offset 5. */
    __u8 out[20] = {0};
    __u16 out_len = 0;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == 20);
    assert(memcmp(out, payload, 20) == 0);

    free(r);
}

static void test_max_payload_round_trip(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    __u8 payload[254];
    for (int j = 0; j < 254; j++) payload[j] = (__u8)(j * 7 + 3);

    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, 254);
    assert(rc == 0);
    r->wr_idx = wr;

    __u8 out[254] = {0};
    __u16 out_len = 0;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == 254);
    assert(memcmp(out, payload, 254) == 0);

    free(r);
}

static void test_zero_length_payload(void) {
    /* A 0-byte payload is legal: just the 2-byte length prefix = 0. Used by some
     * heartbeat/ack patterns; tests that the helpers don't divide-by-len
     * or memcpy with NULL. */
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, NULL, 0);
    assert(rc == 0);
    assert(wr == 2u);   /* just the 2-byte length prefix */
    r->wr_idx = wr;

    __u8 out[1];
    __u16 out_len = 0xFFFF;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == 0);

    free(r);
}

/* 2-byte LE length prefix: a 511-byte payload round-trips (513-byte record). */
static void test_max_record_511(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);
    static __u8 payload[511];
    for (int j = 0; j < 511; j++) payload[j] = (__u8)(j * 5 + 1);

    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, 511);
    assert(rc == 0);
    assert(wr == (__u32)(2u + 511u));      /* 2 length bytes + 511 payload */
    r->wr_idx = wr;

    static __u8 out[511];
    __u16 out_len = 0;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == 511);
    assert(memcmp(out, payload, 511) == 0);
    free(r);
}

/* The 2-byte length prefix itself straddles the end-of-data[] boundary. */
static void test_prefix_wraps_boundary(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);
    /* Put wr 1 byte before the end: low prefix byte at DATA-1, high at 0. */
    __u32 base = RADIO_RING_DATA_BYTES - 1;
    r->wr_idx = base;
    r->rd_idx = base;

    __u8 payload[300];
    for (int j = 0; j < 300; j++) payload[j] = (__u8)(0x40 + (j & 0x3F));
    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, 300);
    assert(rc == 0);
    r->wr_idx = wr;

    /* Low byte of len (300 = 0x012C) at DATA-1, high byte at 0. */
    assert(r->data[RADIO_RING_DATA_BYTES - 1] == 0x2C);
    assert(r->data[0] == 0x01);

    __u8 out[300]; __u16 out_len = 0;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == 300);
    assert(memcmp(out, payload, 300) == 0);
    free(r);
}

/* A record whose payload exceeds 255 wraps the data boundary correctly. */
static void test_large_record_wraps_payload(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);
    __u32 base = RADIO_RING_DATA_BYTES - 100;   /* 2 prefix + ~98 then wrap */
    r->wr_idx = base;
    r->rd_idx = base;

    static __u8 payload[400];
    for (int j = 0; j < 400; j++) payload[j] = (__u8)(j * 3 + 7);
    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, 400);
    assert(rc == 0);
    r->wr_idx = wr;

    static __u8 out[400]; __u16 out_len = 0;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == 400);
    assert(memcmp(out, payload, 400) == 0);
    free(r);
}

/* A record whose length prefix exceeds the max valid payload (511) must be
 * rejected as corrupt, NOT memcpy'd into the consumer buffer. The TX ring is
 * mmap'd writable to userspace, so an oversized len is the heap-overflow vector
 * this clamp closes. */
static void test_oversized_len_rejected(void) {
    struct lr2021_ring *r = calloc(1, sizeof(*r));
    assert(r);

    /* Push a deliberately oversized record (600-byte payload). push_pure only
     * bounds against ring free space, so this lands fine and the prefix says
     * 600 -- exactly what a malformed/hostile userspace producer could write. */
    static __u8 payload[600];
    for (int j = 0; j < 600; j++) payload[j] = (__u8)(j & 0xFF);
    __u32 wr = r->wr_idx;
    int rc = lr2021_ring_push_pure(&wr, r->rd_idx, r->data, payload, 600);
    assert(rc == 0);
    r->wr_idx = wr;

    /* Pop must REJECT it (-1 corrupt) without copying: 600 > 511. */
    static __u8 out[LR2021_RING_MAX_RECORD];
    __u16 out_len = 0xFFFF;
    __u32 rd = r->rd_idx;
    rc = lr2021_ring_pop_pure(r->wr_idx, &rd, r->data, out, &out_len);
    assert(rc == -1);
    assert(out_len == 0xFFFF);   /* not written */

    /* Boundary: exactly 511 (the max valid payload) must still pop cleanly. */
    struct lr2021_ring *r2 = calloc(1, sizeof(*r2));
    assert(r2);
    static __u8 p511[511];
    for (int j = 0; j < 511; j++) p511[j] = (__u8)(j * 9 + 2);
    __u32 wr2 = r2->wr_idx;
    assert(lr2021_ring_push_pure(&wr2, r2->rd_idx, r2->data, p511, 511) == 0);
    r2->wr_idx = wr2;
    __u32 rd2 = r2->rd_idx;
    out_len = 0;
    rc = lr2021_ring_pop_pure(r2->wr_idx, &rd2, r2->data, out, &out_len);
    assert(rc == 1);
    assert(out_len == 511);
    assert(memcmp(out, p511, 511) == 0);

    free(r);
    free(r2);
}

int main(void) {
    test_static_helpers_basic();
    test_empty_pop_returns_zero();
    test_push_pop_round_trip();
    test_push_pop_many_records_fifo();
    test_full_ring_rejects_push();
    test_record_wraps_data_boundary();
    test_uint32_overflow_wraparound();
    test_max_payload_round_trip();
    test_zero_length_payload();
    test_max_record_511();
    test_prefix_wraps_boundary();
    test_large_record_wraps_payload();
    test_oversized_len_rejected();
    printf("All ring buffer tests passed!\n");
    return 0;
}
