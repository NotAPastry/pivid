#include "protocol.h"
#include <string.h>

int protocol_build(uint8_t *out_packet, size_t out_max, proto_mode_t mode,
                   uint8_t flags, const uint8_t *payload, size_t payload_len) {
    if (!out_packet) {
        return -1;
    }
    /* A null payload is OK iff there are no payload bytes to copy. */
    if (!payload && payload_len > 0) {
        return -1;
    }
    if (payload_len > PROTOCOL_MAX_PAYLOAD) {
        return -1;
    }

    size_t hdr = (mode == PROTO_MODE_FIXED) ? (size_t)3 : (size_t)1;
    size_t total = hdr + payload_len;
    if (total > out_max) {
        return -1;
    }

    out_packet[0] = flags;
    if (mode == PROTO_MODE_FIXED) {
        pack_le16(&out_packet[1], (uint16_t)payload_len);
    }
    if (payload_len > 0) {
        memcpy(&out_packet[hdr], payload, payload_len);
    }

    /* total <= 3 + 508 = 511, fits in int. */
    return (int)total;
}

int protocol_parse(const uint8_t *in_packet, size_t in_len, proto_mode_t mode,
                   uint8_t *out_flags, uint8_t *out_payload, size_t *out_payload_len) {
    if (!in_packet || !out_flags || !out_payload || !out_payload_len) {
        return -1;
    }

    size_t hdr = (mode == PROTO_MODE_FIXED) ? (size_t)3 : (size_t)1;
    if (in_len < hdr) {
        return -1;
    }

    size_t length;
    if (mode == PROTO_MODE_FIXED) {
        length = (size_t)unpack_le16(&in_packet[1]);
    } else {
        length = in_len - hdr;            /* derive: packet_len - 1 */
    }

    /* Validate against the payload ceiling AND the input bound before any
     * copy -- a malformed/over-range length must never index past either
     * buffer (belt-and-braces; the chip also flags LenError). */
    if (length > PROTOCOL_MAX_PAYLOAD) {
        return -1;
    }
    if (in_len < hdr + length) {
        return -1;
    }

    *out_flags = in_packet[0];
    *out_payload_len = length;
    if (length > 0) {
        memcpy(out_payload, &in_packet[hdr], length);
    }
    return 0;
}

int protocol_is_bind_packet(const uint8_t *in_packet, size_t in_len,
                            uint32_t *out_session_id, uint32_t *out_sync_word) {
    if (!in_packet || !out_session_id || !out_sync_word) {
        return 0;
    }
    /* Bind packets are always FIXED-mode: flags(1) + len(u16-LE,2) + payload. */
    if (in_len < (size_t)(3 + PROTOCOL_BIND_PAYLOAD_LEN)) {
        return 0;
    }
    uint16_t length = unpack_le16(&in_packet[1]);
    if (length != PROTOCOL_BIND_PAYLOAD_LEN) {
        return 0;
    }

    /* session_id (bytes 3-6) and sync_word (bytes 7-10) as little-endian.
     * Byte-wise inlines avoid strict-aliasing/alignment UB on aarch64. */
    *out_session_id = unpack_le32(&in_packet[3]);
    *out_sync_word  = unpack_le32(&in_packet[7]);
    return 1;
}

void pivid_telemetry_pack(uint8_t out[TELEMETRY_BYTES],
                          const struct pivid_telemetry *t) {
    out[0] = t->version;
    out[1] = t->fc_link_state;
    out[2] = t->voltage_state;
    pack_le16(&out[3],  t->vbat_mv);
    pack_le16(&out[5],  t->mah_drawn);
    pack_le16(&out[7],  (uint16_t)t->amperage_ca);
    pack_le16(&out[9],  t->msp_rssi);
    pack_le16(&out[11], t->throttle_us);
    pack_le32(&out[13], (uint32_t)t->altitude_cm);
    pack_le16(&out[17], (uint16_t)t->roll_decideg);
    pack_le16(&out[19], (uint16_t)t->pitch_decideg);
    pack_le32(&out[21], t->arming_disable_flags);
    pack_le32(&out[25], t->flight_mode_flags);
    out[29] = t->beeper_state;
    pack_le32(&out[30], t->tx_video_packet_total);
    out[34] = t->ctl_lq_pct;
}

int pivid_telemetry_unpack(struct pivid_telemetry *out,
                           const uint8_t in[TELEMETRY_BYTES]) {
    if (in[0] != TELEMETRY_VERSION) return -1;
    out->version               = in[0];
    out->fc_link_state         = in[1];
    out->voltage_state         = in[2];
    out->vbat_mv               = unpack_le16(&in[3]);
    out->mah_drawn             = unpack_le16(&in[5]);
    out->amperage_ca           = (int16_t)unpack_le16(&in[7]);
    out->msp_rssi              = unpack_le16(&in[9]);
    out->throttle_us           = unpack_le16(&in[11]);
    out->altitude_cm           = (int32_t)unpack_le32(&in[13]);
    out->roll_decideg          = (int16_t)unpack_le16(&in[17]);
    out->pitch_decideg         = (int16_t)unpack_le16(&in[19]);
    out->arming_disable_flags  = unpack_le32(&in[21]);
    out->flight_mode_flags     = unpack_le32(&in[25]);
    out->beeper_state          = in[29];
    out->tx_video_packet_total = unpack_le32(&in[30]);
    out->ctl_lq_pct            = in[34];
    return 0;
}
