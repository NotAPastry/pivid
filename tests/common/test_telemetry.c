#include "protocol.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } \
} while (0)

static void test_size_constant(void) {
    EXPECT(TELEMETRY_BYTES == 35, "telemetry payload is 35 bytes");
    EXPECT(TELEMETRY_VERSION == 2, "telemetry version bumped to 2");
}

static void test_roundtrip_full(void) {
    struct pivid_telemetry t = {
        .version = 2, .fc_link_state = 2, .voltage_state = 1,
        .vbat_mv = 16432, .mah_drawn = 412, .amperage_ca = -120,
        .msp_rssi = 871, .throttle_us = 1750, .altitude_cm = -32768,
        .roll_decideg = -1799, .pitch_decideg = 900,
        .arming_disable_flags = 0x80000001u, .flight_mode_flags = 0x00010002u,
        .beeper_state = 0x42, .tx_video_packet_total = 0xDEADBEEFu,
        .ctl_lq_pct = 73,
    };
    uint8_t buf[TELEMETRY_BYTES];
    pivid_telemetry_pack(buf, &t);

    EXPECT(buf[3] == 0x30 && buf[4] == 0x40, "vbat_mv LE bytes @ off 3");
    EXPECT(buf[30] == 0xEF && buf[31] == 0xBE && buf[32] == 0xAD && buf[33] == 0xDE,
           "tx_video_packet_total LE bytes @ off 30");

    struct pivid_telemetry r;
    EXPECT(pivid_telemetry_unpack(&r, buf) == 0, "unpack OK");
    EXPECT(r.version == t.version, "version");
    EXPECT(r.fc_link_state == t.fc_link_state, "fc_link_state");
    EXPECT(r.voltage_state == t.voltage_state, "voltage_state");
    EXPECT(r.vbat_mv == t.vbat_mv, "vbat_mv");
    EXPECT(r.mah_drawn == t.mah_drawn, "mah_drawn");
    EXPECT(r.amperage_ca == t.amperage_ca, "amperage_ca signed edge");
    EXPECT(r.msp_rssi == t.msp_rssi, "msp_rssi");
    EXPECT(r.throttle_us == t.throttle_us, "throttle_us");
    EXPECT(r.altitude_cm == t.altitude_cm, "altitude_cm signed edge");
    EXPECT(r.roll_decideg == t.roll_decideg, "roll_decideg signed edge");
    EXPECT(r.pitch_decideg == t.pitch_decideg, "pitch_decideg");
    EXPECT(r.arming_disable_flags == t.arming_disable_flags, "arming_disable_flags");
    EXPECT(r.flight_mode_flags == t.flight_mode_flags, "flight_mode_flags");
    EXPECT(r.beeper_state == t.beeper_state, "beeper_state");
    EXPECT(r.tx_video_packet_total == t.tx_video_packet_total, "tx_video_packet_total");
    EXPECT(buf[34] == 73, "ctl_lq_pct byte @ off 34");
    EXPECT(r.ctl_lq_pct == t.ctl_lq_pct, "ctl_lq_pct");
}

static void test_ctl_lq_na_sentinel(void) {
    struct pivid_telemetry t = { .version = 2, .ctl_lq_pct = CTL_LQ_NA };
    uint8_t buf[TELEMETRY_BYTES];
    pivid_telemetry_pack(buf, &t);
    EXPECT(buf[34] == 0xFF, "CTL_LQ_NA packs as 0xFF");
    struct pivid_telemetry r;
    EXPECT(pivid_telemetry_unpack(&r, buf) == 0, "unpack OK");
    EXPECT(r.ctl_lq_pct == CTL_LQ_NA, "CTL_LQ_NA survives roundtrip (not 0)");
}

static void test_version_rejected(void) {
    uint8_t buf[TELEMETRY_BYTES] = {0};
    buf[0] = 99;
    struct pivid_telemetry r;
    EXPECT(pivid_telemetry_unpack(&r, buf) != 0, "unpack rejects unknown version");
}

int main(void) {
    test_size_constant();
    test_roundtrip_full();
    test_ctl_lq_na_sentinel();
    test_version_rejected();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("OK\n");
    return 0;
}
