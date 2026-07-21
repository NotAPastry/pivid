/* Host-side fault-injection tests for common/ring_user.{h,c}.
 *
 * Allocates RADIO_RING_BYTES on the heap with aligned_alloc(4096, ...) so
 * the layout matches the kernel-side mmap'd ring exactly. Drives the
 * producer/consumer through the corner cases that historically eat SPSC
 * implementations: wraparound at the data-buffer boundary, and free-running
 * 32-bit index wraparound. */

#include "ring_user.h"
#include "radio_ioctl.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

static void *new_ring(void) {
    void *r = aligned_alloc(4096, RADIO_RING_BYTES);
    assert(r);
    memset(r, 0, RADIO_RING_BYTES);
    return r;
}

/* Direct access to the same uint32_t locations ring_user.h uses. */
static uint32_t *wrp(void *r) { return (uint32_t *)((char *)r + 0);  }
static uint32_t *rdp(void *r) { return (uint32_t *)((char *)r + 64); }

/* --- Case 1: empty ring pop returns 0 --------------------------------- */
static void test_empty_pop(void) {
    void *r = new_ring();
    uint8_t out[256];
    EXPECT(ring_user_pop(r, out, sizeof out) == 0, "empty pop returns 0");
    free(r);
}

/* --- Case 2: push N small records, pop N, byte-for-byte equality ------ */
static void test_push_pop_roundtrip(void) {
    void *r = new_ring();
    const int N = 10;
    uint8_t payloads[10][32];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 32; j++) payloads[i][j] = (uint8_t)(i * 32 + j);
        EXPECT(ring_user_push(r, payloads[i], 32) == 0, "push small record");
    }
    for (int i = 0; i < N; i++) {
        uint8_t out[64] = {0};
        int n = ring_user_pop(r, out, sizeof out);
        EXPECT(n == 32, "pop returns 32");
        EXPECT(memcmp(out, payloads[i], 32) == 0, "pop matches push payload");
    }
    EXPECT(ring_user_pop(r, (uint8_t[256]){0}, 256) == 0, "ring is empty after N pops");
    free(r);
}

/* --- Case 3: fill ring until push fails ------------------------------- */
static void test_push_until_full(void) {
    void *r = new_ring();
    /* Push 100-byte records (102 bytes on-ring with the 2-byte length prefix).
     * Free capacity is RADIO_RING_DATA_BYTES - 1 = 32767. So we can fit
     * floor(32767 / 102) = 321 records, with 32767 - 321*102 = 25 bytes left,
     * which is not enough for another 102-byte record. */
    uint8_t payload[100];
    memset(payload, 0xAA, sizeof payload);
    int pushed = 0;
    while (ring_user_push(r, payload, sizeof payload) == 0) pushed++;
    EXPECT(pushed == 321, "fits exactly 321 records of size 100 before -1");

    /* Verify used == 321 * 102 = 32742, free = 32767 - 32742 = 25 < 102. */
    uint32_t used = *wrp(r) - *rdp(r);
    EXPECT(used == 321u * 102u, "used == 321 * 102");
    EXPECT(RADIO_RING_DATA_BYTES - 1 - used < 102u, "less than one record free");

    /* Drain it and confirm. */
    int popped = 0;
    uint8_t out[256];
    while (ring_user_pop(r, out, sizeof out) > 0) popped++;
    EXPECT(popped == 321, "popped 321 records");
    free(r);
}

/* --- Case 4: wrap across the data-area boundary ----------------------- */
static void test_wrap_data_boundary(void) {
    void *r = new_ring();
    /* Position wr_idx so the next record's 2-byte length prefix lands 6 bytes
     * before the end of the data area; the payload (100 bytes) will straddle
     * the boundary. We arrange this by setting both indices to (DATA - 6)
     * and pushing.
     *
     * "6 bytes before the end" means data[DATA-6,DATA-5] hold the LE length
     * prefix, then payload[0..3] fills data[DATA-4..DATA-1] (4 bytes), and the
     * rest wraps to data[0..]. */
    *wrp(r) = RADIO_RING_DATA_BYTES - 6;
    *rdp(r) = RADIO_RING_DATA_BYTES - 6;

    uint8_t payload[100];
    for (int i = 0; i < 100; i++) payload[i] = (uint8_t)i;
    EXPECT(ring_user_push(r, payload, 100) == 0, "push wrapping record");

    uint8_t *data = (uint8_t *)r + RADIO_RING_HEADER_BYTES;
    EXPECT(data[RADIO_RING_DATA_BYTES - 6] == 100, "len low byte at boundary - 6");
    EXPECT(data[RADIO_RING_DATA_BYTES - 5] == 0,   "len high byte after");
    EXPECT(data[RADIO_RING_DATA_BYTES - 4] == 0,   "first payload byte");
    EXPECT(data[RADIO_RING_DATA_BYTES - 1] == 3,   "fourth payload byte at end");
    EXPECT(data[0]                          == 4,  "fifth payload byte wrapped to 0");
    EXPECT(data[95]                         == 99, "last payload byte at offset 95");

    uint8_t out[256] = {0};
    int n = ring_user_pop(r, out, sizeof out);
    EXPECT(n == 100, "pop returns 100");
    EXPECT(memcmp(out, payload, 100) == 0, "wrapped record reads back intact");
    free(r);
}

/* --- Case 5: free-running 32-bit wrap --------------------------------- */
static void test_index_wrap(void) {
    void *r = new_ring();
    /* Start indices near UINT32_MAX so the pushes carry them across 2^32. */
    *wrp(r) = 0xFFFFFFF0u;
    *rdp(r) = 0xFFFFFFF0u;

    uint8_t payload[7][10];
    for (int i = 0; i < 7; i++) {
        for (int j = 0; j < 10; j++) payload[i][j] = (uint8_t)((i + 1) * 10 + j);
        EXPECT(ring_user_push(r, payload[i], 10) == 0, "push past 2^32 boundary");
    }
    /* After 7 pushes of 12 bytes each, wr_idx has advanced by 84 from
     * 0xFFFFFFF0; in uint32 math that wraps past 2^32 -- confirming the wrap. */
    EXPECT(*wrp(r) == (uint32_t)(0xFFFFFFF0u + 84u),
           "wr_idx wraps in uint32");

    /* Physical offset of the first push: (0xFFFFFFF0 & DATA_MASK) =
     * 0xFFFFFFF0 & 0x7FFF = 0x7FF0. Sanity-check the low length byte landed
     * there. */
    uint8_t *data = (uint8_t *)r + RADIO_RING_HEADER_BYTES;
    EXPECT(data[0xFFFFFFF0u & RADIO_RING_DATA_MASK] == 10,
           "first record's low length byte at expected wrapped offset");

    for (int i = 0; i < 7; i++) {
        uint8_t out[16] = {0};
        int n = ring_user_pop(r, out, sizeof out);
        EXPECT(n == 10, "pop after wrap");
        EXPECT(memcmp(out, payload[i], 10) == 0, "payload survives index wrap");
    }
    free(r);
}

/* --- Case 6: a full 511-byte payload round-trips; 512 is rejected -------- */
static void test_511_byte_record(void) {
    void *r = new_ring();
    static uint8_t payload[511];
    for (int i = 0; i < 511; i++) payload[i] = (uint8_t)(i * 7 + 1);
    EXPECT(ring_user_push(r, payload, 511) == 0, "push 511-byte payload");
    EXPECT(ring_user_push(r, payload, 512) == -1, "512 rejected (>511)");
    static uint8_t out[511];
    int n = ring_user_pop(r, out, sizeof out);
    EXPECT(n == 511, "pop returns 511");
    EXPECT(memcmp(out, payload, 511) == 0, "511-byte record intact");
    free(r);
}

int main(void) {
    test_empty_pop();
    test_push_pop_roundtrip();
    test_push_until_full();
    test_wrap_data_boundary();
    test_index_wrap();
    test_511_byte_record();
    if (g_failures) {
        fprintf(stderr, "test_ring_user: %d failures\n", g_failures);
        return 1;
    }
    printf("test_ring_user: all cases pass\n");
    return 0;
}
