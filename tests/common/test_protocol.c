#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../common/protocol.h"
#include "fec.h"

/* ---- FIXED mode: header is flags(1) + len(u16-LE,2) ------------------ */
static void test_fixed_build_small_nal(void) {
    uint8_t payload[10] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22,0x33,0x44};
    uint8_t out[512];
    uint8_t flags = PROTO_FLAG_START | PROTO_FLAG_END | PROTO_FLAG_SEQ(0);
    int size = protocol_build(out, sizeof out, PROTO_MODE_FIXED, flags, payload, 10);
    assert(size == 13);                 /* flags(1)+len(2)+payload(10) */
    assert(out[0] == flags);
    assert(out[1] == 10 && out[2] == 0);/* u16-LE length = 10 */
    assert(memcmp(&out[3], payload, 10) == 0);
}

static void test_fixed_parse_small_nal(void) {
    uint8_t in[13] = { 0xC0, 0x0A, 0x00,
                       0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x11,0x22,0x33,0x44 };
    uint8_t out_payload[512]; uint8_t out_flags; size_t out_len;
    int rc = protocol_parse(in, sizeof in, PROTO_MODE_FIXED,
                            &out_flags, out_payload, &out_len);
    assert(rc == 0);
    assert(out_flags == 0xC0);
    assert(out_len == 10);
    assert(memcmp(out_payload, &in[3], 10) == 0);
}

/* u16 length > 255 round-trips in FIXED mode (the whole point of the widen). */
static void test_fixed_u16_len_roundtrip(void) {
    enum { N = 300 };
    uint8_t payload[N]; for (int i=0;i<N;i++) payload[i]=(uint8_t)(i*7+1);
    uint8_t out[3 + N];
    int size = protocol_build(out, sizeof out, PROTO_MODE_FIXED, 0xC0, payload, N);
    assert(size == 3 + N);
    assert(out[1] == (uint8_t)(N & 0xFF));        /* 300 & 0xFF = 0x2C */
    assert(out[2] == (uint8_t)((N >> 8) & 0xFF)); /* 300 >> 8  = 0x01 */
    uint8_t back[512]; uint8_t f; size_t l;
    assert(protocol_parse(out, (size_t)size, PROTO_MODE_FIXED, &f, back, &l) == 0);
    assert(l == N && memcmp(back, payload, N) == 0);
}

/* ---- DYNAMIC mode: header is flags(1); no len field ------------------ */
static void test_dynamic_build_no_len(void) {
    uint8_t payload[10] = {0,1,2,3,4,5,6,7,8,9};
    uint8_t out[512];
    int size = protocol_build(out, sizeof out, PROTO_MODE_DYNAMIC, 0xC0, payload, 10);
    assert(size == 11);                 /* flags(1)+payload(10), NO len field */
    assert(out[0] == 0xC0);
    assert(memcmp(&out[1], payload, 10) == 0);
}

static void test_dynamic_parse_derives_len(void) {
    /* RX derives payload extent = packet_len - 1 (no embedded length). */
    uint8_t in[11] = { 0xC0, 0,1,2,3,4,5,6,7,8,9 };
    uint8_t out[512]; uint8_t f; size_t l;
    int rc = protocol_parse(in, sizeof in, PROTO_MODE_DYNAMIC, &f, out, &l);
    assert(rc == 0);
    assert(f == 0xC0);
    assert(l == 10);                    /* 11 - 1 */
    assert(memcmp(out, &in[1], 10) == 0);
}

static void test_dynamic_u16_len_roundtrip(void) {
    enum { N = 400 };
    uint8_t payload[N]; for (int i=0;i<N;i++) payload[i]=(uint8_t)(i*3+5);
    uint8_t out[1 + N];
    int size = protocol_build(out, sizeof out, PROTO_MODE_DYNAMIC, 0xC0, payload, N);
    assert(size == 1 + N);
    uint8_t back[512]; uint8_t f; size_t l;
    assert(protocol_parse(out, (size_t)size, PROTO_MODE_DYNAMIC, &f, back, &l) == 0);
    assert(l == N && memcmp(back, payload, N) == 0);
}

/* ---- Bind packet: ALWAYS the 3-byte FIXED header -------------------- */
static void test_bind_packet_detection(void) {
    uint32_t session_id = 0x12345678;
    uint32_t sync_word  = 0x5A5A5A5A;
    uint8_t payload[PROTOCOL_BIND_PAYLOAD_LEN];
    pack_le32(&payload[0], session_id);
    pack_le32(&payload[4], sync_word);

    uint8_t out[3 + PROTOCOL_BIND_PAYLOAD_LEN];
    uint8_t flags = PROTO_FLAG_START | PROTO_FLAG_END | PROTO_FLAG_SEQ(0);
    int size = protocol_build(out, sizeof out, PROTO_MODE_FIXED, flags,
                              payload, sizeof payload);
    assert(size == (int)sizeof out);    /* 3 header + 8 payload = 11 */

    uint32_t det_sid = 0, det_sw = 0;
    int is_bind = protocol_is_bind_packet(out, (size_t)size, &det_sid, &det_sw);
    assert(is_bind == 1);
    assert(det_sid == session_id);
    assert(det_sw  == sync_word);
}

/* ---- Boundary + safety --------------------------------------------- */
static void test_build_zero_payload(void) {
    uint8_t out[8]; uint8_t buf[1] = {0};
    uint8_t flags = PROTO_FLAG_START | PROTO_FLAG_END | PROTO_FLAG_SEQ(0);
    /* FIXED: flags + 2-byte len(=0) = 3 bytes. */
    int sf = protocol_build(out, sizeof out, PROTO_MODE_FIXED, flags, buf, 0);
    assert(sf == 3 && out[0] == flags && out[1] == 0 && out[2] == 0);
    /* DYNAMIC: just flags = 1 byte. */
    int sd = protocol_build(out, sizeof out, PROTO_MODE_DYNAMIC, flags, buf, 0);
    assert(sd == 1 && out[0] == flags);
}

static void test_build_max_payload(void) {
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
    for (int i=0;i<PROTOCOL_MAX_PAYLOAD;i++) payload[i]=(uint8_t)i;
    uint8_t out[3 + PROTOCOL_MAX_PAYLOAD];
    int size = protocol_build(out, sizeof out, PROTO_MODE_FIXED, 0xC0,
                              payload, PROTOCOL_MAX_PAYLOAD);
    assert(size == 3 + PROTOCOL_MAX_PAYLOAD);
    assert(memcmp(&out[3], payload, PROTOCOL_MAX_PAYLOAD) == 0);
}

static void test_build_oversize_rejected(void) {
    uint8_t payload[PROTOCOL_MAX_PAYLOAD + 1] = {0};
    uint8_t out[3 + PROTOCOL_MAX_PAYLOAD + 1];
    assert(protocol_build(out, sizeof out, PROTO_MODE_FIXED, 0xC0,
                          payload, PROTOCOL_MAX_PAYLOAD + 1) == -1);
    assert(protocol_build(out, sizeof out, PROTO_MODE_DYNAMIC, 0xC0,
                          payload, PROTOCOL_MAX_PAYLOAD + 1) == -1);
}

/* FIXED parse: declared u16 len exceeding the buffer must be rejected
 * with NO out-of-bounds copy (canary check). */
static void test_fixed_parse_overrange_no_oob(void) {
    uint8_t in[10] = { 0xC0, 0xFF, 0xFF, 0,0,0,0,0,0,0 };  /* len = 65535 */
    /* Small output buffer flanked by guard bytes; assert guards untouched. */
    struct { uint8_t guard0; uint8_t out[16]; uint8_t guard1; } b;
    memset(&b, 0xCC, sizeof b);
    uint8_t f; size_t l = 999;
    int rc = protocol_parse(in, sizeof in, PROTO_MODE_FIXED, &f, b.out, &l);
    assert(rc == -1);
    assert(b.guard0 == 0xCC && b.guard1 == 0xCC);  /* no OOB write */
}

/* FIXED parse: declared len > PROTOCOL_MAX_PAYLOAD must be rejected. */
static void test_fixed_parse_over_max_payload(void) {
    /* len = PROTOCOL_MAX_PAYLOAD + 1, with that many bytes actually present. */
    enum { L = PROTOCOL_MAX_PAYLOAD + 1 };
    static uint8_t in[3 + L];
    in[0] = 0xC0; in[1] = (uint8_t)(L & 0xFF); in[2] = (uint8_t)((L >> 8) & 0xFF);
    uint8_t out[512]; uint8_t f; size_t l;
    assert(protocol_parse(in, sizeof in, PROTO_MODE_FIXED, &f, out, &l) == -1);
}

/* DYNAMIC parse: a packet longer than 1 + PROTOCOL_MAX_PAYLOAD is rejected. */
static void test_dynamic_parse_over_max_payload(void) {
    enum { L = 1 + PROTOCOL_MAX_PAYLOAD + 1 };
    static uint8_t in[L] = {0xC0};
    uint8_t out[512]; uint8_t f; size_t l;
    assert(protocol_parse(in, sizeof in, PROTO_MODE_DYNAMIC, &f, out, &l) == -1);
}

static void test_seq_roundtrip(void) {
    for (uint8_t n=0;n<16;n++) {
        assert(PROTO_SEQ_FROM_FLAGS(PROTO_FLAG_SEQ(n)) == n);
        uint8_t f2 = PROTO_FLAG_START|PROTO_FLAG_END|PROTO_FLAG_SEQ(n);
        assert(PROTO_SEQ_FROM_FLAGS(f2) == n);
    }
}

/* ---- FEC pair header (added in Task A2; asserts live here) ---------- */
static void test_fec_pair_subheader(void) {
    assert((PROTO_FLAG_FEC & PROTO_FLAG_START) == 0);
    assert((PROTO_FLAG_FEC & PROTO_FLAG_END) == 0);
    assert((PROTO_FLAG_FEC & (0x0F << 2)) == 0);

    uint8_t hdr[FEC_HDR_BYTES];
    /* count=2: bit7 set; block_id in bits 6:0. */
    proto_fec_pair_hdr_pack(hdr, 0x2B, 7, 16, 19, 2);
    assert(hdr[0] == (0x80 | 0x2B));
    assert(hdr[1] == 7 && hdr[2] == 16 && hdr[3] == 19);
    uint8_t bid, base, k, n, count;
    proto_fec_pair_hdr_unpack(hdr, &bid, &base, &k, &n, &count);
    assert(bid == 0x2B && base == 7 && k == 16 && n == 19 && count == 2);

    /* count=1: bit7 clear. */
    proto_fec_pair_hdr_pack(hdr, 0x7F, 18, 16, 19, 1);
    assert(hdr[0] == 0x7F);
    assert((hdr[0] & 0x80) == 0);                  /* count=1 -> bit7 clear */
    proto_fec_pair_hdr_unpack(hdr, &bid, &base, &k, &n, &count);
    assert(bid == 0x7F && base == 18 && k == 16 && n == 19 && count == 1);

    /* 7-bit block_id boundary: id 0x7F is the max; bit7 is count only and
     * must not bleed into block_id. */
    proto_fec_pair_hdr_pack(hdr, 0x7F, 0, 1, 1, 1);
    assert((hdr[0] & 0x80) == 0 && (hdr[0] & 0x7F) == 0x7F);
    /* count round-trips independent of block_id. */
    proto_fec_pair_hdr_unpack(hdr, &bid, &base, &k, &n, &count);
    assert(bid == 0x7F && count == 1);
}

/* ---- FEC interleave marker ----------------------------------------- */
static void test_fec_ilv_flag(void) {
    /* FEC_ILV is bit 7; it must not collide with FEC/TYPE and is free in FEC pkts. */
    assert(PROTO_FLAG_FEC_ILV == (1u << 7));          /* FEC_ILV is bit 7 (START position) */
    assert((PROTO_FLAG_FEC_ILV & PROTO_FLAG_FEC) == 0);   /* FEC_ILV disjoint from FEC */
    assert((PROTO_FLAG_FEC_ILV & FLAG_TYPE_MASK) == 0);   /* FEC_ILV disjoint from TYPE */
}

int main(void) {
    test_fixed_build_small_nal();
    test_fixed_parse_small_nal();
    test_fixed_u16_len_roundtrip();
    test_dynamic_build_no_len();
    test_dynamic_parse_derives_len();
    test_dynamic_u16_len_roundtrip();
    test_bind_packet_detection();
    test_build_zero_payload();
    test_build_max_payload();
    test_build_oversize_rejected();
    test_fixed_parse_overrange_no_oob();
    test_fixed_parse_over_max_payload();
    test_dynamic_parse_over_max_payload();
    test_seq_roundtrip();
    test_fec_pair_subheader();
    test_fec_ilv_flag();
    printf("All tests passed!\n");
    return 0;
}
