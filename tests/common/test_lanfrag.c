/* Host tests for common/lanfrag.h: header pack/parse round-trip and the
 * NAL chunker. */
#define _POSIX_C_SOURCE 200809L

#include "lanfrag.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
                   g_failures++; } \
} while (0)

/* --- chunk recorder --- */
typedef struct { uint16_t nal_id, frag_off, total_len; size_t payload_len;
                 uint8_t first_payload_byte; } RecChunk;
#define REC_CAP 64
static RecChunk g_rec[REC_CAP];
static size_t   g_rec_n;

static void rec_emit(const uint8_t *hdr, size_t hdr_len,
                     const uint8_t *payload, size_t payload_len, void *user) {
    (void)user;
    EXPECT(hdr_len == LANFRAG_HDR_BYTES, "hdr len");
    EXPECT(payload_len <= LANFRAG_CHUNK_PAYLOAD, "chunk payload <= cap");
    if (g_rec_n >= REC_CAP) return;
    LanfragHdr h;
    /* Re-parse the emitted header by appending one payload byte so dg_len>hdr. */
    uint8_t dg[LANFRAG_HDR_BYTES + 1];
    memcpy(dg, hdr, LANFRAG_HDR_BYTES);
    dg[LANFRAG_HDR_BYTES] = payload_len ? payload[0] : 0;
    EXPECT(lanfrag_parse_hdr(dg, sizeof dg, &h) == 0, "parse emitted hdr");
    RecChunk *r = &g_rec[g_rec_n++];
    r->nal_id = h.nal_id; r->frag_off = h.frag_off; r->total_len = h.total_len;
    r->payload_len = payload_len;
    r->first_payload_byte = payload_len ? payload[0] : 0;
}
static void rec_reset(void) { g_rec_n = 0; memset(g_rec, 0, sizeof g_rec); }

static void test_pack_parse_roundtrip(void) {
    uint8_t hdr[LANFRAG_HDR_BYTES];
    lanfrag_pack_hdr(hdr, 0x1234, 0x2A00, 0xBEEF);
    EXPECT(hdr[0] == 0xA5, "magic");
    EXPECT(hdr[1] == 0x01, "version");
    /* LE byte order */
    EXPECT(hdr[2] == 0x34 && hdr[3] == 0x12, "nal_id LE");
    EXPECT(hdr[4] == 0x00 && hdr[5] == 0x2A, "frag_off LE");
    EXPECT(hdr[6] == 0xEF && hdr[7] == 0xBE, "total_len LE");
    uint8_t dg[LANFRAG_HDR_BYTES + 4] = {0};
    memcpy(dg, hdr, LANFRAG_HDR_BYTES);
    LanfragHdr h;
    EXPECT(lanfrag_parse_hdr(dg, sizeof dg, &h) == 0, "parse ok");
    EXPECT(h.nal_id == 0x1234, "nal_id rt");
    EXPECT(h.frag_off == 0x2A00, "frag_off rt");
    EXPECT(h.total_len == 0xBEEF, "total_len rt");
    EXPECT(h.payload_len == 4, "payload_len");
}

static void test_parse_rejects(void) {
    uint8_t dg[LANFRAG_HDR_BYTES + 1] = {0};
    LanfragHdr h;
    EXPECT(lanfrag_parse_hdr(dg, LANFRAG_HDR_BYTES - 1, &h) == -1, "short rejected");
    lanfrag_pack_hdr(dg, 1, 0, 10);
    dg[0] = 0x00;  /* bad magic (a legacy raw-NAL first byte) */
    EXPECT(lanfrag_parse_hdr(dg, sizeof dg, &h) == -1, "bad magic rejected");
    lanfrag_pack_hdr(dg, 1, 0, 10);
    dg[1] = 0x02;  /* bad version */
    EXPECT(lanfrag_parse_hdr(dg, sizeof dg, &h) == -1, "bad version rejected");
}

static void test_parse_zero_payload(void) {
    /* A datagram of exactly LANFRAG_HDR_BYTES (no payload) is valid and parses
     * with payload_len == 0. The chunker never emits one, but a parser must
     * accept it without reading past the header. */
    uint8_t dg[LANFRAG_HDR_BYTES];
    lanfrag_pack_hdr(dg, 1, 0, 0);
    LanfragHdr h;
    EXPECT(lanfrag_parse_hdr(dg, LANFRAG_HDR_BYTES, &h) == 0, "exact-hdr parses");
    EXPECT(h.payload_len == 0, "zero payload_len");
    EXPECT(h.payload == dg + LANFRAG_HDR_BYTES, "payload at end");
}

static void test_chunk_multi(void) {
    rec_reset();
    /* total 3001 -> chunks: 1400, 1400, 201 */
    uint8_t nal[3001];
    for (size_t i = 0; i < sizeof nal; i++) nal[i] = (uint8_t)(i & 0xFF);
    size_t n = lanfrag_chunk_nal(7, nal, sizeof nal, rec_emit, NULL);
    EXPECT(n == 3, "3 chunks");
    EXPECT(g_rec_n == 3, "3 emits");
    EXPECT(g_rec[0].frag_off == 0 && g_rec[0].payload_len == 1400, "c0");
    EXPECT(g_rec[1].frag_off == 1400 && g_rec[1].payload_len == 1400, "c1");
    EXPECT(g_rec[2].frag_off == 2800 && g_rec[2].payload_len == 201, "c2 tail");
    /* END derivation on last chunk */
    EXPECT(g_rec[2].frag_off + g_rec[2].payload_len == 3001, "END derive");
    /* nal_id + total_len stamped on every chunk; payload offset correct */
    for (size_t i = 0; i < 3; i++) {
        EXPECT(g_rec[i].nal_id == 7, "nal_id");
        EXPECT(g_rec[i].total_len == 3001, "total_len");
    }
    EXPECT(g_rec[1].first_payload_byte == (uint8_t)(1400 & 0xFF), "c1 byte off");
}

static void test_chunk_small_single(void) {
    rec_reset();
    uint8_t nal[100];
    memset(nal, 0x5A, sizeof nal);
    size_t n = lanfrag_chunk_nal(9, nal, sizeof nal, rec_emit, NULL);
    EXPECT(n == 1, "1 chunk");
    EXPECT(g_rec[0].frag_off == 0 && g_rec[0].payload_len == 100, "single");
    EXPECT(g_rec[0].frag_off + g_rec[0].payload_len == 100, "START==END");
}

static void test_chunk_oversize_and_zero(void) {
    rec_reset();
    static uint8_t big[LANFRAG_MAX_NAL + 1];
    EXPECT(lanfrag_chunk_nal(1, big, sizeof big, rec_emit, NULL) == 0, "oversize -> 0");
    EXPECT(g_rec_n == 0, "no emits on oversize");
    EXPECT(lanfrag_chunk_nal(1, big, 0, rec_emit, NULL) == 0, "zero -> 0");
    /* exactly MAX is allowed */
    rec_reset();
    size_t n = lanfrag_chunk_nal(1, big, LANFRAG_MAX_NAL, rec_emit, NULL);
    EXPECT(n == (LANFRAG_MAX_NAL + LANFRAG_CHUNK_PAYLOAD - 1) / LANFRAG_CHUNK_PAYLOAD,
           "max chunk count");
}

int main(void) {
    test_pack_parse_roundtrip();
    test_parse_rejects();
    test_parse_zero_payload();
    test_chunk_multi();
    test_chunk_small_single();
    test_chunk_oversize_and_zero();
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all lanfrag tests passed\n");
    return 0;
}
