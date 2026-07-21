/* tests/player/test_mdns_parse.c -- unit tests for the pure mDNS parser.
 *
 * golden_pkt is a REAL 207-byte Avahi response captured 2026-06-24 from the live
 * caster (pivid-rx) for a QU query of _pivid._udp.local. Structure:
 *   PTR(PiVid._pivid._udp.local) TXT SRV(port 5700 -> pivid-rx.local) AAAA AAAA
 *   A(192.168.1.23), 6 answers, SRV-before-A, multi-level name compression.
 * It validates the PARSER against real wire bytes (not live discovery). */
#include "mdns_parse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define C_IN 1   /* DNS class IN, mirrored from mdns_parse.c (internal there) */

static const uint8_t golden_pkt[207] = {
    0x00, 0x00, 0x84, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x06, 0x5f, 0x70, 0x69, 0x76, 0x69, 0x64, 0x04, 0x5f, 0x75, 0x64, 0x70,
    0x05, 0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x00, 0x00, 0x0c, 0x80, 0x01, 0xc0,
    0x0c, 0x00, 0x0c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x08, 0x05,
    0x50, 0x69, 0x56, 0x69, 0x64, 0xc0, 0x0c, 0xc0, 0x2f, 0x00, 0x10, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x27, 0x0a, 0x63, 0x6f, 0x64, 0x65,
    0x63, 0x3d, 0x68, 0x32, 0x36, 0x34, 0x09, 0x77, 0x69, 0x64, 0x74, 0x68,
    0x3d, 0x36, 0x34, 0x30, 0x0a, 0x68, 0x65, 0x69, 0x67, 0x68, 0x74, 0x3d,
    0x34, 0x38, 0x30, 0x06, 0x66, 0x70, 0x73, 0x3d, 0x33, 0x30, 0xc0, 0x2f,
    0x00, 0x21, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x11, 0x00, 0x00,
    0x00, 0x00, 0x16, 0x44, 0x08, 0x70, 0x69, 0x76, 0x69, 0x64, 0x2d, 0x72,
    0x78, 0xc0, 0x18, 0xc0, 0x7c, 0x00, 0x1c, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x0a, 0x00, 0x10, 0xfd, 0x96, 0x2b, 0x81, 0xfc, 0x7a, 0x00, 0x08, 0x2e,
    0xcf, 0x67, 0xff, 0xfe, 0x94, 0xb5, 0xa1, 0xc0, 0x7c, 0x00, 0x1c, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x10, 0x2a, 0x0d, 0x33, 0x41, 0xba,
    0x6b, 0x3c, 0x08, 0x2e, 0xcf, 0x67, 0xff, 0xfe, 0x94, 0xb5, 0xa1, 0xc0,
    0x7c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x04, 0xc0,
    0xa8, 0x01, 0x17,
};

static void test_golden(void) {
    MdnsAcc acc; memset(&acc, 0, sizeof acc);
    bool done = mdns_parse_response(golden_pkt, sizeof golden_pkt, "_pivid._udp", &acc);
    assert(done);
    assert(acc.have_srv && acc.have_a);
    assert(strcmp(acc.host, "pivid-rx.local") == 0);
    assert(acc.port == 5700);
    assert(strcmp(acc.ipv4, "192.168.1.23") == 0);
    printf("test_golden PASS\n");
}

/* ---- tiny uncompressed-packet builder (test-only) ---------------------- */
typedef struct { uint8_t b[512]; size_t n; } Buf;
static void put8(Buf *x, uint8_t v)   { x->b[x->n++] = v; }
static void put16(Buf *x, uint16_t v) { put8(x, (uint8_t)(v >> 8)); put8(x, (uint8_t)v); }
static void putmem(Buf *x, const void *p, size_t n) { memcpy(x->b + x->n, p, n); x->n += n; }
static void putname(Buf *x, const char *name) {       /* labels, no compression */
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        put8(x, (uint8_t)l); putmem(x, s, l);
        s += l; if (dot) s++;
    }
    put8(x, 0);
}
static void put_srv(Buf *x, const char *owner, uint16_t cls,
                    uint16_t port, const char *target) {
    Buf rd = {{0}, 0};
    put16(&rd, 0); put16(&rd, 0); put16(&rd, port);   /* prio, weight, port */
    putname(&rd, target);
    putname(x, owner); put16(x, 33); put16(x, cls);
    put16(x, 0); put16(x, 10);                         /* TTL = 10 */
    put16(x, (uint16_t)rd.n); putmem(x, rd.b, rd.n);
}
static void put_a(Buf *x, const char *owner, uint16_t cls, const char *ip4 /*4 bytes*/) {
    putname(x, owner); put16(x, 1); put16(x, cls);
    put16(x, 0); put16(x, 10);
    put16(x, 4); putmem(x, ip4, 4);
}
static void put_hdr(Buf *x, uint16_t an) {
    put16(x, 0); put16(x, 0x8400); put16(x, 0); put16(x, an);
    put16(x, 0); put16(x, 0);                          /* QD=0 NS=0 AR=0 */
}

/* Cache-flush bit (0x8000) set on the RR CLASS must be masked, not treated as a
 * different class -- otherwise every valid Avahi RR is dropped. */
static void test_cache_flush_bit(void) {
    Buf p = {{0}, 0};
    put_hdr(&p, 2);
    put_srv(&p, "PiVid._pivid._udp.local", 0x8000 | C_IN, 5700, "host.local");
    put_a(&p, "host.local", 0x8000 | C_IN, "\xc0\xa8\x01\x17");  /* 192.168.1.23 */
    MdnsAcc acc; memset(&acc, 0, sizeof acc);
    assert(mdns_parse_response(p.b, p.n, "_pivid._udp", &acc));
    assert(acc.port == 5700 && strcmp(acc.ipv4, "192.168.1.23") == 0);
    printf("test_cache_flush_bit PASS\n");
}

/* RR order is not guaranteed: an A arriving before its SRV must still reconcile. */
static void test_a_before_srv(void) {
    Buf p = {{0}, 0};
    put_hdr(&p, 2);
    put_a(&p, "host.local", C_IN, "\x0a\x00\x00\x05");            /* 10.0.0.5 */
    put_srv(&p, "PiVid._pivid._udp.local", C_IN, 6000, "host.local");
    MdnsAcc acc; memset(&acc, 0, sizeof acc);
    assert(mdns_parse_response(p.b, p.n, "_pivid._udp", &acc));
    assert(acc.have_srv && acc.have_a);
    assert(acc.port == 6000 && strcmp(acc.ipv4, "10.0.0.5") == 0);
    printf("test_a_before_srv PASS\n");
}

/* An A whose owner != the SRV target must NOT satisfy have_a. */
static void test_owner_mismatch(void) {
    Buf p = {{0}, 0};
    put_hdr(&p, 2);
    put_srv(&p, "PiVid._pivid._udp.local", C_IN, 5700, "right.local");
    put_a(&p, "wrong.local", C_IN, "\x01\x02\x03\x04");
    MdnsAcc acc; memset(&acc, 0, sizeof acc);
    assert(!mdns_parse_response(p.b, p.n, "_pivid._udp", &acc));
    assert(acc.have_srv && !acc.have_a);
    printf("test_owner_mismatch PASS\n");
}

/* A different service's SRV is ignored (service filter). */
static void test_service_filter(void) {
    Buf p = {{0}, 0};
    put_hdr(&p, 2);
    put_srv(&p, "Other._http._tcp.local", C_IN, 80, "host.local");
    put_a(&p, "host.local", C_IN, "\x01\x02\x03\x04");
    MdnsAcc acc; memset(&acc, 0, sizeof acc);
    assert(!mdns_parse_response(p.b, p.n, "_pivid._udp", &acc));
    assert(!acc.have_srv);
    printf("test_service_filter PASS\n");
}

/* Every truncation of the golden packet must be total: no crash, no over-read
 * (run this binary under ASan locally), result simply incomplete. */
static void test_truncation(void) {
    for (size_t n = 0; n <= sizeof golden_pkt; n++) {
        MdnsAcc acc; memset(&acc, 0, sizeof acc);
        bool done = mdns_parse_response(golden_pkt, n, "_pivid._udp", &acc);
        if (n < sizeof golden_pkt) assert(!done);     /* only the full packet completes */
    }
    printf("test_truncation PASS\n");
}

/* A self/forward compression pointer must be rejected (no infinite loop). */
static void test_pointer_loop(void) {
    /* header(AN=1) then an RR whose owner name is a pointer to offset 12 (itself). */
    Buf p = {{0}, 0};
    put_hdr(&p, 1);
    put8(&p, 0xC0); put8(&p, 0x0C);                   /* ptr -> 12 == its own offset */
    put16(&p, 1); put16(&p, C_IN); put16(&p, 0); put16(&p, 10); put16(&p, 0);
    MdnsAcc acc; memset(&acc, 0, sizeof acc);
    assert(!mdns_parse_response(p.b, p.n, "_pivid._udp", &acc));   /* returns, no hang */
    printf("test_pointer_loop PASS\n");
}

int main(void) {
    test_golden();
    test_cache_flush_bit();
    test_a_before_srv();
    test_owner_mismatch();
    test_service_filter();
    test_truncation();
    test_pointer_loop();
    printf("ALL test_mdns_parse PASS\n");
    return 0;
}
