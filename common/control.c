/* apps/pivid-caster/src/control.c -- see control.h for the wire layout. */
#include "control.h"

static void w8 (uint8_t *p, size_t *o, uint8_t  v) { p[(*o)++] = v; }
static void w16(uint8_t *p, size_t *o, uint16_t v) { p[(*o)++] = (uint8_t)v; p[(*o)++] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, size_t *o, uint32_t v) {
    p[(*o)++] = (uint8_t)v;         p[(*o)++] = (uint8_t)(v >> 8);
    p[(*o)++] = (uint8_t)(v >> 16); p[(*o)++] = (uint8_t)(v >> 24);
}

static uint16_t r16(const uint8_t *p, size_t o) { return (uint16_t)(p[o] | (p[o + 1] << 8)); }
static uint32_t r32(const uint8_t *p, size_t o) {
    return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8)
         | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
}

static size_t write_header(uint8_t *buf, uint8_t type) {
    size_t o = 0;
    w32(buf, &o, CASTER_CTRL_MAGIC);
    w8 (buf, &o, (uint8_t)CASTER_CTRL_VERSION);
    w8 (buf, &o, type);
    return o;  /* == CASTER_CTRL_HEADER_LEN */
}

size_t caster_ctrl_build_subscribe(uint8_t *buf, size_t cap,
                                   uint16_t video_port, uint16_t telem_port) {
    if (cap < CASTER_CTRL_SUBSCRIBE_LEN) return 0;
    size_t o = write_header(buf, (uint8_t)CASTER_MSG_SUBSCRIBE);
    w16(buf, &o, video_port);
    w16(buf, &o, telem_port);
    return o;  /* == CASTER_CTRL_SUBSCRIBE_LEN */
}

size_t caster_ctrl_build_unsubscribe(uint8_t *buf, size_t cap) {
    if (cap < CASTER_CTRL_UNSUBSCRIBE_LEN) return 0;
    return write_header(buf, (uint8_t)CASTER_MSG_UNSUBSCRIBE);
}

int caster_ctrl_parse(const uint8_t *buf, size_t len, CasterCtrlMsg *out) {
    if (len < CASTER_CTRL_HEADER_LEN)          return -1;
    if (r32(buf, 0) != CASTER_CTRL_MAGIC)      return -1;
    if (buf[4] != CASTER_CTRL_VERSION)         return -1;
    uint8_t type = buf[5];

    out->type = type;
    out->video_port = 0;
    out->telem_port = 0;

    if (type == CASTER_MSG_SUBSCRIBE) {
        if (len < CASTER_CTRL_SUBSCRIBE_LEN)   return -1;
        out->video_port = r16(buf, 6);
        out->telem_port = r16(buf, 8);
        return 0;
    }
    if (type == CASTER_MSG_UNSUBSCRIBE) {
        return 0;   /* header-only; trailing bytes ignored */
    }
    return -1;  /* unknown type */
}
