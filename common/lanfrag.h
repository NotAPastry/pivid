/* common/lanfrag.h -- LAN-hop video chunk wire format (caster -> player).
 *
 * The caster splits each framed NAL ([00 00 00 01] + NAL, <= 64 KB) into
 * datagrams that never IP-fragment; the player reassembles by byte offset and
 * forwards the clean contiguous prefix of a partial NAL to its conceal-in-place
 * decoder. Offset-addressed (not the radio's 4-bit SEQ) so out-of-order delivery
 * and partial-forward both work. All multi-byte fields are little-endian via the
 * byte-shift helpers in protocol.h (no aliasing casts; aarch64-safe).
 *
 * 8-byte header:
 *   off 0  u8   magic     0xA5  (!= 0x00, so a legacy raw NAL is distinguishable)
 *   off 1  u8   version   0x01
 *   off 2  u16  nal_id    rolling per-NAL id (wraps 65536)
 *   off 4  u16  frag_off  byte offset of this chunk within the framed NAL
 *   off 6  u16  total_len total framed-NAL length (<= 65535)
 *   off 8  ...  payload   <= LANFRAG_CHUNK_PAYLOAD framed-NAL bytes
 * START = (frag_off == 0); END = (frag_off + payload_len == total_len). */
#ifndef COMMON_LANFRAG_H
#define COMMON_LANFRAG_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"   /* pack_le16 / unpack_le16 */

#define LANFRAG_MAGIC          0xA5u
#define LANFRAG_VERSION        0x01u
#define LANFRAG_HDR_BYTES      8u
#define LANFRAG_CHUNK_PAYLOAD  1400u   /* +8 hdr = 1408 < 1472 (1500 MTU - 28) */
#define LANFRAG_MAX_NAL        65535u  /* u16 total_len ceiling */

typedef struct {
    uint16_t       nal_id;
    uint16_t       frag_off;
    uint16_t       total_len;
    const uint8_t *payload;       /* points into the input datagram */
    size_t         payload_len;
} LanfragHdr;

/* Write the 8-byte header into out (>= LANFRAG_HDR_BYTES). */
static inline void lanfrag_pack_hdr(uint8_t *out, uint16_t nal_id,
                                    uint16_t frag_off, uint16_t total_len) {
    out[0] = (uint8_t)LANFRAG_MAGIC;
    out[1] = (uint8_t)LANFRAG_VERSION;
    pack_le16(out + 2, nal_id);
    pack_le16(out + 4, frag_off);
    pack_le16(out + 6, total_len);
}

/* Parse a received datagram. Returns 0 on success (fills *out), -1 on a short
 * datagram or bad magic/version. */
static inline int lanfrag_parse_hdr(const uint8_t *dg, size_t dg_len,
                                    LanfragHdr *out) {
    if (dg_len < LANFRAG_HDR_BYTES)            return -1;
    if (dg[0] != (uint8_t)LANFRAG_MAGIC)       return -1;
    if (dg[1] != (uint8_t)LANFRAG_VERSION)     return -1;
    out->nal_id      = unpack_le16(dg + 2);
    out->frag_off    = unpack_le16(dg + 4);
    out->total_len   = unpack_le16(dg + 6);
    out->payload     = dg + LANFRAG_HDR_BYTES;
    out->payload_len = dg_len - LANFRAG_HDR_BYTES;
    return 0;
}

/* Per-chunk emit callback: header (LANFRAG_HDR_BYTES) + a slice of the NAL. */
typedef void (*LanfragEmitFn)(const uint8_t *hdr, size_t hdr_len,
                              const uint8_t *payload, size_t payload_len,
                              void *user);

/* Split a framed NAL into chunks, invoking emit() per chunk in ascending
 * offset order. Returns the chunk count, or 0 if total_len is 0 or
 * > LANFRAG_MAX_NAL (oversize; caller counts + drops). */
static inline size_t lanfrag_chunk_nal(uint16_t nal_id, const uint8_t *nal,
                                       size_t total_len, LanfragEmitFn emit,
                                       void *user) {
    if (total_len == 0 || total_len > LANFRAG_MAX_NAL) return 0;
    uint8_t hdr[LANFRAG_HDR_BYTES];
    size_t off = 0, chunks = 0;
    do {
        size_t take = total_len - off;
        if (take > LANFRAG_CHUNK_PAYLOAD) take = LANFRAG_CHUNK_PAYLOAD;
        lanfrag_pack_hdr(hdr, nal_id, (uint16_t)off, (uint16_t)total_len);
        emit(hdr, sizeof hdr, nal + off, take, user);
        off += take;
        chunks++;
    } while (off < total_len);
    return chunks;
}

#endif
