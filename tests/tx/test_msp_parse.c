#include "msp_parse.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } \
} while (0)

static uint8_t crc(uint8_t size, uint8_t cmd, const uint8_t *p, size_t n) {
    uint8_t c = size ^ cmd;
    for (size_t i = 0; i < n; i++) c ^= p[i];
    return c;
}
static int feed(MSP_Parser *p, const uint8_t *b, size_t n) {
    int last = 0;
    for (size_t i = 0; i < n; i++) { last = msp_parser_feed(p, b[i]); if (last == 1) return 1; }
    return last;
}

static void test_zero_payload(void) {
    MSP_Parser p; msp_parser_init(&p);
    uint8_t b[] = { '$','M','>',0,150, crc(0,150,NULL,0) };
    EXPECT(feed(&p, b, sizeof b) == 1, "zero-payload frame delivered");
    EXPECT(p.cmd == 150 && p.size == 0, "cmd/size preserved");
}
static void test_typical_payload(void) {
    uint8_t pl[10] = {1,2,3,4,5,6,7,8,9,10};
    uint8_t b[16]; b[0]='$';b[1]='M';b[2]='>';b[3]=10;b[4]=130;
    memcpy(&b[5], pl, 10); b[15] = crc(10,130,pl,10);
    MSP_Parser p; msp_parser_init(&p);
    EXPECT(feed(&p, b, sizeof b) == 1, "10-byte payload delivered");
    EXPECT(p.cmd == 130 && p.size == 10, "cmd/size");
    EXPECT(memcmp(p.payload, pl, 10) == 0, "payload bytes preserved");
}
static void test_crc_failure(void) {
    uint8_t b[] = { '$','M','>',2,42, 0xAA,0xBB, 0x55 /*wrong*/ };
    MSP_Parser p; msp_parser_init(&p);
    EXPECT(feed(&p, b, sizeof b) == -1, "CRC mismatch returns -1");
    EXPECT(p.crc_failures == 1, "crc_failures incremented");
    EXPECT(p.state == MSP_WAIT_DOLLAR, "parser reset after bad CRC");
}
static void test_truncated_then_clean(void) {
    /* A truncated frame (size says 5, only 2 payload bytes arrive) is now resolved
     * by length+CRC, not by a mid-frame '$' watchdog: the parser keeps consuming
     * bytes as payload, the misframed CRC fails, and it re-arms at the next '$'.
     * Recovery therefore costs the first clean frame after the truncation; the
     * SECOND clean frame delivers. (On the real clean wired UART, crc_failures==0,
     * so truncation never actually occurs -- see msp_parse.c.) */
    MSP_Parser p; msp_parser_init(&p);
    uint8_t junk[] = { '$','M','>',5,99,1,2 };
    for (size_t i=0;i<sizeof junk;i++) msp_parser_feed(&p, junk[i]);
    uint8_t good1[] = { '$','M','>',0,1, crc(0,1,NULL,0) };   /* eaten by the misframe */
    uint8_t good2[] = { '$','M','>',0,2, crc(0,2,NULL,0) };   /* clean resync delivers */
    for (size_t i=0;i<sizeof good1;i++) msp_parser_feed(&p, good1[i]);
    EXPECT(feed(&p, good2, sizeof good2) == 1, "recovers on the second clean frame");
    EXPECT(p.cmd == 2, "recovered cmd=2");
    EXPECT(p.crc_failures >= 1, "misframed CRC counted while resyncing");
}
static void test_garbage_prefix(void) {
    uint8_t b[10]; b[0]=0x01;b[1]=0xFF;b[2]='M';b[3]='<';
    b[4]='$';b[5]='M';b[6]='>';b[7]=0;b[8]=42; b[9]=crc(0,42,NULL,0);
    MSP_Parser p; msp_parser_init(&p);
    EXPECT(feed(&p, b, sizeof b) == 1, "skips garbage prefix");
    EXPECT(p.cmd == 42, "garbage-prefix cmd=42");
}
static void test_back_to_back(void) {
    uint8_t b[] = { '$','M','>',0,1,crc(0,1,NULL,0), '$','M','>',0,2,crc(0,2,NULL,0) };
    MSP_Parser p; msp_parser_init(&p);
    int got1=0, got2=0;
    for (size_t i=0;i<sizeof b;i++) { if (msp_parser_feed(&p,b[i])==1){ if(p.cmd==1)got1=1; if(p.cmd==2)got2=1; } }
    EXPECT(got1 && got2, "both back-to-back frames delivered");
}

static void test_max_payload(void) {
    /* 255-byte payload guards the buffer-full '>=' boundary in READ_PAYLOAD.
     * The deterministic fill intentionally includes 0x24 ('$') bytes to prove a
     * length-delimited payload stores them verbatim rather than re-arming. */
    uint8_t pl[255];
    for (int i = 0; i < 255; i++) pl[i] = (uint8_t)(i*7+1);   /* hits 0x24 at i=5, ... */
    uint8_t b[5+255+1];
    b[0]='$';b[1]='M';b[2]='>';b[3]=255;b[4]=200;
    memcpy(&b[5], pl, 255); b[5+255] = crc(255,200,pl,255);
    MSP_Parser p; msp_parser_init(&p);
    EXPECT(feed(&p, b, sizeof b) == 1, "255-byte payload delivered");
    EXPECT(p.cmd == 200 && p.size == 255, "max cmd/size");
    EXPECT(memcmp(p.payload, pl, 255) == 0, "max payload bytes preserved");
}
static void test_dollar_in_payload(void) {
    /* MSPv1 is length-delimited: once size+cmd are read, every payload byte is
     * stored verbatim -- including 0x24 ('$') -- and CRC validates the frame. A
     * '$' only re-arms while hunting the preamble, so a payload '$' is NOT a
     * re-sync. This is the common case for BF DisplayPort (glyph bytes span the
     * full 0..255 range) and was the source of the bulk of parse_resets. */
    uint8_t pl[3] = { 0x24, 0x01, 0x02 };
    uint8_t b[5+3+1];
    b[0]='$';b[1]='M';b[2]='>';b[3]=3;b[4]=50;
    memcpy(&b[5], pl, 3); b[5+3] = crc(3,50,pl,3);
    MSP_Parser p; msp_parser_init(&p);
    EXPECT(feed(&p, b, sizeof b) == 1, "frame with '$' in payload IS delivered");
    EXPECT(p.cmd == 50 && p.size == 3, "cmd/size preserved");
    EXPECT(memcmp(p.payload, pl, 3) == 0, "payload incl 0x24 preserved");
    EXPECT(p.parse_resets == 0, "no re-sync counted for payload '$'");
}
static void test_dollar_as_crc(void) {
    /* The CRC byte itself can legitimately be 0x24; READ_CRC must compare it, not
     * treat it as a preamble re-arm. payload {0x25}, size 1, cmd 0 -> crc 0x24. */
    uint8_t pl[1] = { 0x25 };
    uint8_t b[5+1+1];
    b[0]='$';b[1]='M';b[2]='>';b[3]=1;b[4]=0;
    b[5]=pl[0]; b[6]=crc(1,0,pl,1);
    EXPECT(b[6] == 0x24, "test vector: crc byte is 0x24");
    MSP_Parser p; msp_parser_init(&p);
    EXPECT(feed(&p, b, sizeof b) == 1, "frame with 0x24 CRC byte IS delivered");
    EXPECT(p.parse_resets == 0, "no re-sync counted for 0x24 CRC");
}

int main(void) {
    test_zero_payload(); test_typical_payload(); test_crc_failure();
    test_truncated_then_clean(); test_garbage_prefix(); test_back_to_back();
    test_max_payload(); test_dollar_in_payload(); test_dollar_as_crc();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("OK\n");
    return 0;
}
