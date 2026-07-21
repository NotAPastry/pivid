#include "protocol.h"
#include <stdbool.h>
#include <stdio.h>

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } \
} while (0)

static void test_video_roundtrip(void) {
    /* TYPE=0 (video) must be bit-identical to the legacy PROTO_FLAG_* layout so
     * existing receivers keep working. SEQ occupies bits 5..2; bit 1 is FEC;
     * bit 0 is the new TYPE. pivid_pack_flags for VIDEO must leave bits 1 and 0
     * clear. */
    for (int seq = 0; seq < 16; seq++) {
        uint8_t legacy = PROTO_FLAG_START | PROTO_FLAG_END | PROTO_FLAG_SEQ(seq);
        uint8_t neu    = pivid_pack_flags(true, true, (uint8_t)seq, PKT_VIDEO);
        EXPECT(legacy == neu, "video flag byte matches legacy PROTO_FLAG_* layout");

        bool s = false, e = false; uint8_t out_seq = 0xFF;
        enum pivid_packet_type t = PKT_OSD_GRID;
        pivid_unpack_flags(neu, &s, &e, &out_seq, &t);
        EXPECT(s && e, "start/end roundtrip");
        EXPECT(out_seq == (uint8_t)seq, "seq roundtrip");
        EXPECT(t == PKT_VIDEO, "type=video roundtrip");
    }
}

static void test_telemetry_flags(void) {
    uint8_t b = pivid_pack_flags(true, true, 0, PKT_OSD_GRID);
    EXPECT((b & (1u << 0)) != 0, "telemetry sets bit 0");
    EXPECT((b & PROTO_FLAG_FEC) == 0, "telemetry leaves FEC (bit 1) clear");

    bool s = false, e = false; uint8_t seq = 0xFF;
    enum pivid_packet_type t = PKT_VIDEO;
    pivid_unpack_flags(b, &s, &e, &seq, &t);
    EXPECT(s && e && seq == 0, "telemetry start/end/seq");
    EXPECT(t == PKT_OSD_GRID, "telemetry type=1");
}

static void test_fec_does_not_corrupt_type(void) {
    /* A video packet carried under FEC sets bit 1 but not bit 0 -> decodes as
     * TYPE=VIDEO. */
    uint8_t b = pivid_pack_flags(true, false, 5, PKT_VIDEO) | PROTO_FLAG_FEC;
    bool s, e; uint8_t seq; enum pivid_packet_type t;
    pivid_unpack_flags(b, &s, &e, &seq, &t);
    EXPECT(t == PKT_VIDEO, "FEC bit does not flip TYPE decode");
    EXPECT(seq == 5, "FEC bit does not corrupt SEQ decode");
}

int main(void) {
    test_video_roundtrip();
    test_telemetry_flags();
    test_fec_does_not_corrupt_type();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("OK\n");
    return 0;
}
