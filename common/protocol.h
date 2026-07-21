#ifndef COMMON_PROTOCOL_H
#define COMMON_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Largest payload one outer packet may carry. With the FLRC 511 B container:
 * FIXED header = flags(1)+len(2) = 3 -> 511-3 = 508. */
#define PROTOCOL_MAX_PAYLOAD 508
#define PROTOCOL_BIND_PAYLOAD_LEN 8

#define PROTO_FLAG_START (1 << 7)
#define PROTO_FLAG_END   (1 << 6)
#define PROTO_FLAG_SEQ(n) (((n) & 0x0F) << 2)
#define PROTO_SEQ_FROM_FLAGS(flags) (((flags) >> 2) & 0x0F)

/* FEC layer (transport sublayer below reassembly). One of the two free flag
 * bits; bit0 stays reserved for the planned MSP/OSD TYPE bit.
 * Bit map: START=1<<7, END=1<<6, SEQ=bits5..2, FEC=1<<1, reserved=1<<0. */
#define PROTO_FLAG_FEC (1 << 1)

/* Interleave marker (FEC packets only): when set, the pair sub-header's id field
 * is block_A and byte1 is the shared column index j; the packet carries
 * (block_A[j], block_B[j]) with block_B=(block_A+1)&0x7F. Reuses bit 7 (the
 * START position) — START/END/SEQ are all unused in a FEC outer packet, so this
 * never collides. Legacy FEC packets leave it clear; an old RX ignores it. */
#define PROTO_FLAG_FEC_ILV (1 << 7)

/* Packet TYPE bit = bit 0 of the flags byte (bit 1 is PROTO_FLAG_FEC):
 *   bit:  7      6     5  4  3  2     1     0
 *        START  END  ─── SEQ ──      FEC   TYPE
 * v2.1 video packets leave bit 0 clear, so they decode as TYPE=VIDEO
 * (bit-identical, no flag day). */
enum pivid_packet_type {
    PKT_VIDEO    = 0,
    PKT_OSD_GRID = 1,   /* was PKT_TELEMETRY; same wire bit, new payload */
};

#define FLAG_TYPE_MASK (1u << 0)   /* bit 0 — NOT bit 1 (that is FEC) */

static inline uint8_t pivid_pack_flags(bool start, bool end, uint8_t seq4,
                                       enum pivid_packet_type type) {
    uint8_t b = 0;
    if (start) b |= PROTO_FLAG_START;
    if (end)   b |= PROTO_FLAG_END;
    b |= PROTO_FLAG_SEQ(seq4);
    if (type == PKT_OSD_GRID) b |= FLAG_TYPE_MASK;
    return b;
}

static inline void pivid_unpack_flags(uint8_t b, bool *start, bool *end,
                                      uint8_t *seq4, enum pivid_packet_type *type) {
    if (start) *start = (b & PROTO_FLAG_START) != 0;
    if (end)   *end   = (b & PROTO_FLAG_END)   != 0;
    if (seq4)  *seq4  = (uint8_t)PROTO_SEQ_FROM_FLAGS(b);
    if (type)  *type  = (b & FLAG_TYPE_MASK) ? PKT_OSD_GRID : PKT_VIDEO;
}

/* Outer-packet framing mode (matches the FLRC pkt_format value).
 *   DYNAMIC: chip emits a 9-bit on-air length header; protocol header is
 *            flags(1) only -- RX derives payload extent from packet length.
 *   FIXED:   payload padded to 511 on air; protocol header is flags(1)+len(2).
 * A running link uses EXACTLY one mode (configured out-of-band on both ends);
 * the RX never guesses the mode from packet contents. */
typedef enum { PROTO_MODE_DYNAMIC = 0, PROTO_MODE_FIXED = 1 } proto_mode_t;

/* On-wire FEC pair sub-header (4 bytes): packs 1 or 2 RS symbols' framing.
 *   byte0 = (count==2 ? 0x80 : 0x00) | (block_id & 0x7F)
 *           // bit7=count(1|2), bits6:0=block_id
 *   byte1 = base_idx   (index of the first symbol in the RS block)
 *   byte2 = k          (RS data-symbol count)
 *   byte3 = n          (= k + m, total RS symbols)
 * count (bit7) is folded into byte0 so a count=1 tail still fits 256 B in FIXED
 * mode (header stays 4 bytes). block_id is 7-bit (wraps at 128); TX masks &0x7F.
 * There is NO per-packet mode bit: a TX/RX mode mismatch is caught downstream by
 * the pair-header sanity check + strict length equality (loud mass drops). */
#define FEC_HDR_BYTES 4

static inline void proto_fec_pair_hdr_pack(uint8_t *p, uint8_t block_id,
                                           uint8_t base_idx, uint8_t k,
                                           uint8_t n, uint8_t count) {
    p[0] = (uint8_t)((count == 2 ? 0x80 : 0x00)
                   | (block_id & 0x7F));
    p[1] = base_idx;
    p[2] = k;
    p[3] = n;
}
static inline void proto_fec_pair_hdr_unpack(const uint8_t *p, uint8_t *block_id,
                                             uint8_t *base_idx, uint8_t *k,
                                             uint8_t *n, uint8_t *count) {
    *count    = (uint8_t)((p[0] & 0x80) ? 2 : 1);
    *block_id = (uint8_t)(p[0] & 0x7F);
    *base_idx = p[1];
    *k        = p[2];
    *n        = p[3];
}

/* Little-endian wire-format helpers.
 * Avoid `*(uint32_t*)p` casts: on aarch64 they are strict-aliasing UB and the
 * compiler may assume natural alignment. These byte-wise inlines compile to
 * the same code as an aligned load on aarch64 at -O2 but are well-defined. */
static inline uint16_t unpack_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline void pack_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline uint32_t unpack_le32(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}
static inline void pack_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v        & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

#define TELEMETRY_BYTES   35
#define TELEMETRY_VERSION 2

/* Control-link LQ percentage sentinel: rendered as "--". 0 is a *real* LQ
 * reading (total loss), so the "no data" value must be one the FC never reports
 * for a valid 0..100 percent. Same value (0xFF) as OSD_VID_LQ_NA in osd_grid.h. */
#define CTL_LQ_NA 0xFFu

/* Wire layout, little-endian. See design "Telemetry payload (TYPE=1 packets)". */
struct pivid_telemetry {
    uint8_t  version;
    uint8_t  fc_link_state;
    uint8_t  voltage_state;
    uint16_t vbat_mv;
    uint16_t mah_drawn;
    int16_t  amperage_ca;
    uint16_t msp_rssi;
    uint16_t throttle_us;
    int32_t  altitude_cm;
    int16_t  roll_decideg;
    int16_t  pitch_decideg;
    uint32_t arming_disable_flags;
    uint32_t flight_mode_flags;
    uint8_t  beeper_state;
    uint32_t tx_video_packet_total;
    uint8_t  ctl_lq_pct;            /* 0..100; CTL_LQ_NA(0xFF) = no data -> "--" */
};

void pivid_telemetry_pack(uint8_t out[TELEMETRY_BYTES],
                          const struct pivid_telemetry *t);
/* Returns 0 on success, -1 if version != TELEMETRY_VERSION. */
int  pivid_telemetry_unpack(struct pivid_telemetry *out,
                            const uint8_t in[TELEMETRY_BYTES]);

/* Build one outer packet for the given mode. Returns the total packet size,
 * or -1 on bad args / over-range payload / insufficient out_max.
 *   FIXED:   out = [flags][len u16-LE][payload]   (3-byte header)
 *   DYNAMIC: out = [flags][payload]               (1-byte header) */
int protocol_build(uint8_t *out_packet, size_t out_max, proto_mode_t mode,
                   uint8_t flags, const uint8_t *payload, size_t payload_len);

/* Parse one received outer packet for the given mode. Validates the payload
 * length against PROTOCOL_MAX_PAYLOAD and the input bound BEFORE any copy;
 * rejects over-range with -1 and performs no out-of-bounds access.
 *   FIXED:   payload_len from the embedded u16-LE len field.
 *   DYNAMIC: payload_len = in_len - 1 (derived). */
int protocol_parse(const uint8_t *in_packet, size_t in_len, proto_mode_t mode,
                   uint8_t *out_flags, uint8_t *out_payload, size_t *out_payload_len);

/* Detect a bind packet. Bind packets ALWAYS use the FIXED-mode header
 * (self-describing during pre-config bind): reads u16-LE len at bytes[1..2],
 * payload starts at byte[3], requires len == PROTOCOL_BIND_PAYLOAD_LEN. */
int protocol_is_bind_packet(const uint8_t *in_packet, size_t in_len,
                            uint32_t *out_session_id, uint32_t *out_sync_word);

#endif
