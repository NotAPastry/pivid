/* Unit test for the caster control codec. Byte-exact LE + round-trip + reject. */
#include "control.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_build_subscribe_bytes(void) {
    uint8_t buf[CASTER_CTRL_SUBSCRIBE_LEN];
    size_t n = caster_ctrl_build_subscribe(buf, sizeof buf, 5000, 5001);
    assert(n == CASTER_CTRL_SUBSCRIBE_LEN);
    assert(rd_u32(buf + 0) == CASTER_CTRL_MAGIC);
    assert(buf[4] == CASTER_CTRL_VERSION);
    assert(buf[5] == CASTER_MSG_SUBSCRIBE);
    assert(rd_u16(buf + 6) == 5000);
    assert(rd_u16(buf + 8) == 5001);
    printf("test_build_subscribe_bytes PASS\n");
}

static void test_build_unsubscribe_bytes(void) {
    uint8_t buf[CASTER_CTRL_UNSUBSCRIBE_LEN];
    size_t n = caster_ctrl_build_unsubscribe(buf, sizeof buf);
    assert(n == CASTER_CTRL_UNSUBSCRIBE_LEN);
    assert(rd_u32(buf + 0) == CASTER_CTRL_MAGIC);
    assert(buf[4] == CASTER_CTRL_VERSION);
    assert(buf[5] == CASTER_MSG_UNSUBSCRIBE);
    printf("test_build_unsubscribe_bytes PASS\n");
}

static void test_build_too_small(void) {
    uint8_t small[CASTER_CTRL_SUBSCRIBE_LEN - 1];
    assert(caster_ctrl_build_subscribe(small, sizeof small, 1, 2) == 0);
    uint8_t small2[CASTER_CTRL_UNSUBSCRIBE_LEN - 1];
    assert(caster_ctrl_build_unsubscribe(small2, sizeof small2) == 0);
    printf("test_build_too_small PASS\n");
}

static void test_roundtrip(void) {
    uint8_t buf[CASTER_CTRL_SUBSCRIBE_LEN];
    caster_ctrl_build_subscribe(buf, sizeof buf, 40000, 40001);
    CasterCtrlMsg m;
    assert(caster_ctrl_parse(buf, sizeof buf, &m) == 0);
    assert(m.type == CASTER_MSG_SUBSCRIBE);
    assert(m.video_port == 40000 && m.telem_port == 40001);

    uint8_t ub[CASTER_CTRL_UNSUBSCRIBE_LEN];
    caster_ctrl_build_unsubscribe(ub, sizeof ub);
    CasterCtrlMsg u;
    assert(caster_ctrl_parse(ub, sizeof ub, &u) == 0);
    assert(u.type == CASTER_MSG_UNSUBSCRIBE);
    printf("test_roundtrip PASS\n");
}

static void test_reject_bad(void) {
    uint8_t buf[CASTER_CTRL_SUBSCRIBE_LEN];
    caster_ctrl_build_subscribe(buf, sizeof buf, 5000, 5001);
    CasterCtrlMsg m;

    /* Bad magic. */
    uint8_t b1[CASTER_CTRL_SUBSCRIBE_LEN]; memcpy(b1, buf, sizeof b1); b1[0] ^= 0xFF;
    assert(caster_ctrl_parse(b1, sizeof b1, &m) == -1);
    /* Bad version. */
    uint8_t b2[CASTER_CTRL_SUBSCRIBE_LEN]; memcpy(b2, buf, sizeof b2); b2[4] = 99;
    assert(caster_ctrl_parse(b2, sizeof b2, &m) == -1);
    /* Unknown type. */
    uint8_t b3[CASTER_CTRL_SUBSCRIBE_LEN]; memcpy(b3, buf, sizeof b3); b3[5] = 9;
    assert(caster_ctrl_parse(b3, sizeof b3, &m) == -1);
    /* Truncated header. */
    assert(caster_ctrl_parse(buf, 3, &m) == -1);
    /* SUBSCRIBE truncated to header length (missing ports). */
    assert(caster_ctrl_parse(buf, CASTER_CTRL_HEADER_LEN, &m) == -1);
    printf("test_reject_bad PASS\n");
}

static void test_forward_compat_trailing(void) {
    /* A future, longer SUBSCRIBE: old parser reads the known prefix. */
    uint8_t big[CASTER_CTRL_SUBSCRIBE_LEN + 4];
    caster_ctrl_build_subscribe(big, CASTER_CTRL_SUBSCRIBE_LEN, 5000, 5001);
    big[CASTER_CTRL_SUBSCRIBE_LEN + 0] = 0xAA;
    big[CASTER_CTRL_SUBSCRIBE_LEN + 1] = 0xBB;
    big[CASTER_CTRL_SUBSCRIBE_LEN + 2] = 0xCC;
    big[CASTER_CTRL_SUBSCRIBE_LEN + 3] = 0xDD;
    CasterCtrlMsg m;
    assert(caster_ctrl_parse(big, sizeof big, &m) == 0);
    assert(m.type == CASTER_MSG_SUBSCRIBE && m.video_port == 5000 && m.telem_port == 5001);
    printf("test_forward_compat_trailing PASS\n");
}

int main(void) {
    test_build_subscribe_bytes();
    test_build_unsubscribe_bytes();
    test_build_too_small();
    test_roundtrip();
    test_reject_bad();
    test_forward_compat_trailing();
    printf("ALL test_caster_control PASS\n");
    return 0;
}
