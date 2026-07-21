#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../common/settings.h"

static void test_plan5_fields(void) {
    Settings s;
    settings_load_from_file(&s, "/nonexistent");   /* defaults */
    char buf[64];

    /* ap_password default is "broadcast" */
    assert(settings_get_field_str(&s, "PIVID_AP_PASSWORD", buf, sizeof buf) == 0);
    assert(strcmp(buf, "broadcast") == 0);
    assert(settings_set_field_str(&s, "PIVID_AP_PASSWORD", "hunter2pass") == 0);
    assert(strcmp(s.ap_password, "hunter2pass") == 0);

    /* frequency is now settable; out-of-range is REJECTED [1.9e9, 2.5e9] */
    assert(settings_set_field_str(&s, "RF_FREQUENCY", "2400000000") == 0);
    assert(s.rf_frequency == 2400000000ull);
    assert(settings_set_field_str(&s, "RF_FREQUENCY", "999") == -1); /* out of range */
    assert(s.rf_frequency == 2400000000ull);                              /* unchanged */

    /* tx_power settable; over-max is REJECTED (<= 12) */
    assert(settings_set_field_str(&s, "RF_TX_POWER", "6") == 0);
    assert(s.rf_tx_power == 6);
    assert(settings_set_field_str(&s, "RF_TX_POWER", "30") == -1);  /* over max */
    assert(s.rf_tx_power == 6);                                           /* unchanged */
    printf("test_plan5_fields: OK\n");
}

static void test_osd_field_str(void) {
    Settings s;
    settings_load_from_file(&s, "/nonexistent");   /* defaults */
    char buf[256];

    /* osd_keyframe_ms: default 200, get/set round-trip */
    assert(settings_get_field_str(&s, "OSD_KEYFRAME_MS", buf, sizeof buf) == 0);
    assert(strcmp(buf, "200") == 0);
    assert(settings_set_field_str(&s, "OSD_KEYFRAME_MS", "300") == 0);
    assert(s.osd_keyframe_ms == 300);
    assert(settings_get_field_str(&s, "OSD_KEYFRAME_MS", buf, sizeof buf) == 0);
    assert(strcmp(buf, "300") == 0);

    /* RSSI cell: default "1,1", set/get canonical "row,col" */
    assert(settings_get_field_str(&s, "OSD_VID_RSSI_CELL", buf, sizeof buf) == 0);
    assert(strcmp(buf, "1,1") == 0);
    assert(settings_set_field_str(&s, "OSD_VID_RSSI_CELL", "2,3") == 0);
    assert(s.osd_vid_rssi_row == 2 && s.osd_vid_rssi_col == 3);
    assert(settings_get_field_str(&s, "OSD_VID_RSSI_CELL", buf, sizeof buf) == 0);
    assert(strcmp(buf, "2,3") == 0);

    /* LQ cell: default "2,1", set/get canonical "row,col" */
    assert(settings_get_field_str(&s, "OSD_VID_LQ_CELL", buf, sizeof buf) == 0);
    assert(strcmp(buf, "2,1") == 0);
    assert(settings_set_field_str(&s, "OSD_VID_LQ_CELL", "4,5") == 0);
    assert(s.osd_vid_lq_row == 4 && s.osd_vid_lq_col == 5);
    assert(settings_get_field_str(&s, "OSD_VID_LQ_CELL", buf, sizeof buf) == 0);
    assert(strcmp(buf, "4,5") == 0);

    /* cell out-of-range now REJECTED, value unchanged from last good (2,3) */
    assert(settings_set_field_str(&s, "OSD_VID_RSSI_CELL", "99,99") == -1);
    assert(s.osd_vid_rssi_row == 2 && s.osd_vid_rssi_col == 3);
    assert(settings_set_field_str(&s, "OSD_VID_RSSI_CELL", "-1,-1") == -1);
    assert(s.osd_vid_rssi_row == 2 && s.osd_vid_rssi_col == 3);

    printf("test_osd_field_str: OK\n");
}

static void test_player_local_field_str(void) {
    /* PLAYER_LOCAL: RX-only on/off, default off, get/set round-trips. */
    Settings s; settings_load_from_file(&s, "/nonexistent");
    char buf[SETTINGS_FIELD_STR_MAX];
    assert(settings_get_field_str(&s, "PLAYER_LOCAL", buf, sizeof buf) == 0);
    assert(strcmp(buf, "0") == 0);                       /* default off */
    assert(settings_set_field_str(&s, "PLAYER_LOCAL", "1") == 0);
    assert(s.player_local == 1);
    assert(settings_set_field_str(&s, "PLAYER_LOCAL", "2") == -1); /* out of range */
    printf("test_player_local_field_str: OK\n");
}

int main(void) {
    Settings s;
    settings_load_from_file(&s, "/nonexistent");   /* defaults */
    char buf[64];

    assert(settings_get_field_str(&s, "BITRATE", buf, sizeof buf) == 0);
    assert(strcmp(buf, "850000") == 0);
    assert(settings_get_field_str(&s, "ENCODER_RECIPE", buf, sizeof buf) == 0);
    assert(strcmp(buf, "droppable") == 0);
    assert(settings_get_field_str(&s, "NOPE", buf, sizeof buf) == -1);

    assert(settings_set_field_str(&s, "BITRATE", "1200000") == 0);
    assert(s.bitrate == 1200000);
    assert(settings_set_field_str(&s, "BINDING_PHRASE", "tall-orange-falcon") == 0);
    assert(strcmp(s.binding_phrase, "tall-orange-falcon") == 0);
    assert(settings_set_field_str(&s, "FEC_M_ANCHOR", "4") == 0 && s.fec_m_anchor == 4);
    /* out-of-range left unchanged */
    assert(settings_set_field_str(&s, "FPS", "9999") == -1 && s.fps == 30);
    assert(settings_set_field_str(&s, "NOPE", "x") == -1);

    /* round-trip via get after set */
    assert(settings_get_field_str(&s, "FEC_M_ANCHOR", buf, sizeof buf) == 0);
    assert(strcmp(buf, "4") == 0);
    printf("All field_str tests passed!\n");

    test_plan5_fields();
    test_osd_field_str();
    test_player_local_field_str();
    return 0;
}
