#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../common/settings.h"
#include "../common/phrase_sync.h"
#include "../common/recipe_defaults.h"

/* Round-trip every field of a fully-populated Settings through save+load. */
static void test_settings_round_trip(void) {
    const char *path = "/tmp/pivid_test_pivid_roundtrip.conf";
    unlink(path);

    Settings saved;
    memset(&saved, 0, sizeof(saved));
    strncpy(saved.binding_phrase, "tall-orange-falcon", SETTINGS_PHRASE_MAX - 1);
    saved.flrc_cr = 1; saved.flrc_crc_width = 1;
    saved.flrc_preamble_len = 7; saved.flrc_sync_len = 2;
    saved.flrc_pld_len = 480; saved.flrc_tx_low_thresh = 192;
    saved.rf_frequency = 2028000000ull; saved.rf_tx_power = 12; /* 4 MHz-grid aligned */
    saved.rx_boost = 7;
    saved.width = 1280; saved.height = 720; saved.fps = 25;
    saved.bitrate = 1310000;
    strncpy(saved.recipe, "gdr", SETTINGS_RECIPE_MAX - 1);
    saved.intra = 0; saved.slice_mb = 240; saved.droppable_len = 4; saved.sps_reinject_ms = 1000;
    saved.idr_period = 60;
    saved.fec_m_anchor = 4; saved.fec_m_ref = 1; saved.fec_k = 16;
    saved.forward_to_pc = 1;
    saved.player_local = 1;

    assert(settings_save_to_file(&saved, path) == 0);

    Settings l;
    memset(&l, 0xFF, sizeof(l));   /* poison so a missed parse can't hide */
    assert(settings_load_from_file(&l, path) == 0);

    assert(strcmp(l.binding_phrase, saved.binding_phrase) == 0);
    assert(l.flrc_cr == 1 && l.flrc_crc_width == 1);
    assert(l.flrc_preamble_len == 7 && l.flrc_sync_len == 2);
    assert(l.flrc_pld_len == 480 && l.flrc_tx_low_thresh == 192);
    assert(l.rf_frequency == 2028000000ull && l.rf_tx_power == 12);
    assert(l.rx_boost == 7);
    assert(l.width == 1280 && l.height == 720 && l.fps == 25);
    assert(l.bitrate == 1310000);
    assert(strcmp(l.recipe, "gdr") == 0);
    assert(l.intra == 0 && l.slice_mb == 240 && l.droppable_len == 4 && l.sps_reinject_ms == 1000);
    assert(l.idr_period == 60);
    assert(l.fec_m_anchor == 4 && l.fec_m_ref == 1 && l.fec_k == 16);
    assert(l.forward_to_pc == 1);
    assert(l.player_local == 1);
    /* sync_word is derived, never serialized, but must reproduce. */
    assert(l.sync_word == phrase_to_sync_word("tall-orange-falcon"));
    assert(l.sync_word != 0);

    unlink(path);
}

/* The serializer leaves no temp file behind and produces a loadable file. */
static void test_settings_atomic_no_tempfile(void) {
    const char *path = "/tmp/pivid_test_pivid_atomic.conf";
    unlink(path);

    Settings s;
    settings_load_from_file(&s, "/tmp/does_not_exist_xyz.conf"); /* gives defaults */
    assert(settings_save_to_file(&s, path) == 0);

    /* The target exists and reloads; no <path>.tmpXXXXXX siblings remain. */
    assert(access(path, F_OK) == 0);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "ls %s.tmp* >/dev/null 2>&1", path);
    assert(system(cmd) != 0);   /* glob matches nothing -> ls fails */

    unlink(path);
}

/* Save into a directory that doesn't exist: mkstemp fails, so the call returns
 * -1 and leaves no temp file anywhere. Exercises the error path the happy-path
 * leak check can't reach. */
static void test_settings_save_error_path(void) {
    const char *path = "/tmp/pivid_no_such_dir_xyz/foo.conf";
    assert(settings_save_to_file(NULL, path) == -1);      /* NULL guard       */
    assert(settings_save_to_file((Settings *)1, NULL) == -1); /* NULL path     */

    Settings s;
    settings_load_from_file(&s, "/tmp/does_not_exist_xyz.conf");
    assert(settings_save_to_file(&s, path) == -1);        /* mkstemp fails     */
    /* Nothing was created under the (absent) directory. */
    assert(access(path, F_OK) != 0);
}

/* A binding phrase longer than the buffer must truncate SAFELY and never keep
 * a stray opening quote (quotes are stripped before truncation). */
static void test_settings_long_phrase_truncates(void) {
    const char *path = "/tmp/pivid_test_pivid_longphrase.conf";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    /* 80 'A's inside quotes -> well over SETTINGS_PHRASE_MAX (64). */
    fputs("BINDING_PHRASE=\"", f);
    for (int i = 0; i < 80; i++) fputc('A', f);
    fputs("\"\n", f);
    fclose(f);

    Settings s;
    assert(settings_load_from_file(&s, path) == 0);
    assert(strlen(s.binding_phrase) == SETTINGS_PHRASE_MAX - 1);
    assert(s.binding_phrase[0] == 'A');   /* NOT a stray leading quote */
    unlink(path);
}

/* Missing file -> defaults, including the derived default sync word. */
static void test_settings_defaults_when_missing(void) {
    const char *path = "/tmp/pivid_test_pivid_missing.conf";
    unlink(path);

    Settings s;
    memset(&s, 0xFF, sizeof(s));
    assert(settings_load_from_file(&s, path) == 0);

    assert(strcmp(s.binding_phrase, DEFAULT_BINDING_PHRASE) == 0);
    assert(s.sync_word == phrase_to_sync_word(DEFAULT_BINDING_PHRASE));
    assert(s.width == 640 && s.height == 480 && s.fps == 30);
    assert(s.bitrate == 850000);
    assert(strcmp(s.recipe, "droppable") == 0);
    assert(s.idr_period == 60);
    assert(s.fec_m_anchor == 9 && s.fec_m_ref == 2 && s.fec_k == 16);
    assert(s.forward_to_pc == 0);
}

/* settings_get_sync_word: NULL -> DEFAULT_SYNC_WORD; non-NULL -> derived. */
static void test_settings_get_sync_word(void) {
    assert(settings_get_sync_word(NULL) == DEFAULT_SYNC_WORD);
    Settings s;
    settings_load_from_file(&s, "/tmp/does_not_exist_xyz.conf");
    assert(settings_get_sync_word(&s) == phrase_to_sync_word(DEFAULT_BINDING_PHRASE));
}

/* Quote-stripping + CRLF + section banners + unknown keys are all handled. */
static void test_settings_parse_robustness(void) {
    const char *path = "/tmp/pivid_test_pivid_robust.conf";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs("# --- [pairing] ---\r\n", f);            /* banner, CRLF */
    fputs("BINDING_PHRASE=\"quick-brown-fox\"\n", f); /* quoted */
    fputs("\n", f);                                  /* blank line */
    fputs("ENCODER_RECIPE=gdr\r\n", f);        /* unquoted + CRLF */
    fputs("MSP_UART_DEVICE=\"/dev/ttyS3\"\n", f);
    fputs("TOTALLY_UNKNOWN=whatever\n", f);    /* ignored */
    fputs("FPS=24\n", f);
    fclose(f);

    Settings s;
    assert(settings_load_from_file(&s, path) == 0);
    assert(strcmp(s.binding_phrase, "quick-brown-fox") == 0);
    assert(strcmp(s.recipe, "gdr") == 0);
    assert(strcmp(s.msp_uart_device, "/dev/ttyS3") == 0);
    assert(s.fps == 24);
    unlink(path);
}

/* Malformed/out-of-range numerics leave the default in place (no silent 0). */
static void test_settings_rejects_malformed(void) {
    const char *path = "/tmp/pivid_test_pivid_bad.conf";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs("MSP_BAUD=99999999\n", f);   /* out of range */
    fputs("FPS=not-a-number\n", f);    /* malformed    */
    fputs("FLRC_PLD_LEN=5\n", f);      /* below min 6  */
    fclose(f);

    Settings s;
    assert(settings_load_from_file(&s, path) == 0);
    assert(s.msp_baud == 115200);    /* default preserved */
    assert(s.fps == 30);
    assert(s.flrc_pld_len == 511);
    unlink(path);
}

/* OSD config keys: parse, defaults, and bounds. */
static void test_settings_osd_keys(void) {
    const char *path = "/tmp/pivid_test_osd_keys.conf";
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs("OSD_KEYFRAME_MS=250\n", f);
    fputs("OSD_VID_RSSI_CELL=2,3\n", f);
    fputs("OSD_VID_LQ_CELL=4,5\n", f);
    fclose(f);

    Settings s;
    assert(settings_load_from_file(&s, path) == 0);
    assert(s.osd_keyframe_ms == 250);
    assert(s.osd_vid_rssi_row == 2 && s.osd_vid_rssi_col == 3);
    assert(s.osd_vid_lq_row == 4  && s.osd_vid_lq_col == 5);
    unlink(path);

    /* Defaults when no keys present. */
    Settings d;
    memset(&d, 0xFF, sizeof(d));
    assert(settings_load_from_file(&d, "/tmp/does_not_exist_osd.conf") == 0);
    assert(d.osd_keyframe_ms == 200);
    assert(d.osd_vid_rssi_row == 1 && d.osd_vid_rssi_col == 1);
    assert(d.osd_vid_lq_row == 2  && d.osd_vid_lq_col == 1);

    /* Out-of-range keyframe_ms (below 20) leaves default. */
    FILE *f2 = fopen(path, "w");
    assert(f2 != NULL);
    fputs("OSD_KEYFRAME_MS=5\n", f2);    /* below min 20 */
    fclose(f2);
    Settings r;
    assert(settings_load_from_file(&r, path) == 0);
    assert(r.osd_keyframe_ms == 200);           /* default preserved */
    unlink(path);

    printf("test_settings_osd_keys: OK\n");
}

static void test_settings_defaults_direct(void) {
    Settings s; memset(&s, 0xAA, sizeof s);
    settings_defaults(&s);
    assert(s.flrc_cr == 1 && s.flrc_pld_len == 511);
    assert(s.rf_frequency == 2100000000ull && s.rf_tx_power == -3); /* default 2100 MHz (on the 4 MHz FE-cal grid), -3 dBm PA drive */
    assert(s.width == 640 && s.height == 480 && s.fps == 30);
    assert(s.bitrate == 850000 && s.fec_m_anchor == 9 && s.fec_m_ref == 2 && s.fec_k == 16);
    assert(s.slice_mb == 120 && s.droppable_len == 8 && s.sps_reinject_ms == 0);
    assert(strcmp(s.recipe, "droppable") == 0);
    assert(strcmp(s.ap_password, "broadcast") == 0);
    assert(s.binding_phrase[0] != '\0');
    /* AQM default must be "enforce" (enforce-on-by-default). */
    assert(strcmp(s.aqm, "enforce") == 0);
}

/* AQM: default enforce, get/set round-trip, and save→load round-trip. */
static void test_settings_aqm(void) {
    /* Default is "enforce". */
    Settings s;
    settings_defaults(&s);
    assert(strcmp(s.aqm, "enforce") == 0);

    /* settings_get_field_str returns the current value. */
    char buf[32];
    assert(settings_get_field_str(&s, "AQM", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "enforce") == 0);

    /* settings_set_field_str("0") then get returns "0". */
    assert(settings_set_field_str(&s, "AQM", "0") == 0);
    assert(settings_get_field_str(&s, "AQM", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "0") == 0);

    /* Save "enforce" → load fresh → still "enforce". */
    const char *path = "/tmp/pivid_test_aqm.conf";
    unlink(path);
    settings_defaults(&s);  /* aqm = "enforce" */
    assert(settings_save_to_file(&s, path) == 0);
    Settings l;
    memset(&l, 0xFF, sizeof(l));
    assert(settings_load_from_file(&l, path) == 0);
    assert(strcmp(l.aqm, "enforce") == 0);
    unlink(path);
}

/* The dashboard Reset button writes settings_defaults(); the recipe autofill
 * writes recipe_defaults[]. They MUST agree for the default recipe (droppable)
 * or a Reset silently ships a config the validation rejected. This guard fails
 * CI if they ever drift apart again. */
static void test_defaults_match_validated_recipe(void) {
    Settings s; settings_defaults(&s);
    assert(strcmp(s.recipe, "droppable") == 0);
    int checked = 0;
    for (size_t i = 0; i < recipe_defaults_len; i++) {
        const RecipeDefault *d = &recipe_defaults[i];
        if (strcmp(d->recipe, "droppable") != 0) continue;
        int want = atoi(d->value);
        if      (strcmp(d->key, "BITRATE") == 0)         { assert(s.bitrate == want); checked++; }
        else if (strcmp(d->key, "SLICE_MB") == 0)        { assert(s.slice_mb == want); checked++; }
        else if (strcmp(d->key, "INTRA") == 0)           { assert(s.intra == want); checked++; }
        else if (strcmp(d->key, "DROPPABLE_LEN") == 0)   { assert(s.droppable_len == want); checked++; }
        else if (strcmp(d->key, "FEC_M_ANCHOR") == 0)    { assert(s.fec_m_anchor == want); checked++; }
        else if (strcmp(d->key, "FEC_M_REF") == 0)       { assert(s.fec_m_ref == want); checked++; }
        else if (strcmp(d->key, "FEC_K") == 0)           { assert(s.fec_k == want); checked++; }
        else if (strcmp(d->key, "SPS_REINJECT_MS") == 0) { assert(s.sps_reinject_ms == want); checked++; }
        else if (strcmp(d->key, "IDR_PERIOD") == 0)      { assert(s.idr_period == want); checked++; }
    }
    assert(checked == 9);   /* every droppable key was compared */
}

/* "FEC always on": m_anchor/m_ref must reject 0 (and negatives) at the setter. */
static void test_fec_m_rejects_zero(void) {
    Settings s; settings_defaults(&s);
    assert(settings_set_field_str(&s, "FEC_M_ANCHOR", "0") == -1);
    assert(settings_set_field_str(&s, "FEC_M_REF", "0") == -1);
    /* valid values still accepted, and the rejected writes left defaults intact */
    assert(s.fec_m_anchor == 9 && s.fec_m_ref == 2);   /* m_ref default is 2 (blocky-artifact fix) */
    assert(settings_set_field_str(&s, "FEC_M_REF", "3") == 0 && s.fec_m_ref == 3);
}

/* IMP-5: CRC OFF (code 0) must be rejected at the config layer -- RS erasure
 * decode can't detect a corrupted-but-present symbol, so the chip CRC is the
 * only corruption detector. Kernel param still accepts 0 for bench use. */
static void test_crc_width_rejects_off(void) {
    Settings s; settings_defaults(&s);
    assert(s.flrc_crc_width == 1);                                /* default 16-bit */
    assert(settings_set_field_str(&s, "FLRC_CRC_WIDTH", "0") == -1);
    assert(s.flrc_crc_width == 1);                                /* untouched */
    assert(settings_set_field_str(&s, "FLRC_CRC_WIDTH", "2") == 0 && s.flrc_crc_width == 2);
    assert(settings_set_field_str(&s, "FLRC_CRC_WIDTH", "3") == 0 && s.flrc_crc_width == 3);
}

/* RX_BOOST (SetRxPath front-end boost, IMP-3 prep): default 4 (non-boosted,
 * docs/DECISIONS.md); config layer accepts only the offered 4..7 (0-3 sit at or
 * below the HF reference gain -- kernel param still takes 0..7 for bench use,
 * mirroring the FLRC_CRC_WIDTH OFF split); outside it rejected keep-old. */
static void test_rx_boost_range(void) {
    Settings s; settings_defaults(&s);
    assert(s.rx_boost == 4);                                       /* default */
    assert(settings_set_field_str(&s, "RX_BOOST", "5") == 0 && s.rx_boost == 5);
    assert(settings_set_field_str(&s, "RX_BOOST", "7") == 0 && s.rx_boost == 7);
    assert(settings_set_field_str(&s, "RX_BOOST", "3") == -1);     /* below the offered range */
    assert(s.rx_boost == 7);                                       /* untouched */
    assert(settings_set_field_str(&s, "RX_BOOST", "8") == -1);
    assert(s.rx_boost == 7);                                       /* untouched */
    assert(settings_set_field_str(&s, "RX_BOOST", "-1") == -1);
    assert(s.rx_boost == 7);                                       /* untouched */
}

/* RF_FREQUENCY snaps DOWN to the 4 MHz FE-cal grid on set + load, and the
 * default is already grid-aligned. Guards the chip-alignment invariant. */
static void test_rf_freq_snaps_to_calib_grid(void) {
    /* pure helper */
    assert(snap_rf_freq_to_calib_grid(2030000000ull) == 2028000000ull); /* down */
    assert(snap_rf_freq_to_calib_grid(2028000000ull) == 2028000000ull); /* on-grid */
    assert(snap_rf_freq_to_calib_grid(2031999999ull) == 2028000000ull); /* just under 2032 */
    assert(snap_rf_freq_to_calib_grid(2032000000ull) == 2032000000ull);
    assert(snap_rf_freq_to_calib_grid(1900000000ull) == 1900000000ull); /* min is on-grid */

    /* default is aligned */
    Settings s; settings_defaults(&s);
    assert(s.rf_frequency == 2100000000ull);

    /* set path snaps */
    assert(settings_set_field_str(&s, "RF_FREQUENCY", "2030000000") == 0);
    assert(s.rf_frequency == 2028000000ull);

    /* get_field reflects the snapped value (what the dashboard shows) */
    char buf[SETTINGS_FIELD_STR_MAX];
    assert(settings_get_field_str(&s, "RF_FREQUENCY", buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "2028000000") == 0);
}

/* PLAYER_COMPOSITE / PLAYER_TV_STANDARD: defaults, set/get, range validation,
 * and file round-trip. Mirrors PLAYER_LOCAL wiring. */
static void test_composite_roundtrip(void) {
    Settings s; settings_defaults(&s);
    /* defaults: HDMI (composite off), PAL */
    assert(s.player_composite == 0);
    assert(s.player_tv_standard == 0);

    assert(settings_set_field_str(&s, "PLAYER_COMPOSITE", "1") == 0);
    assert(settings_set_field_str(&s, "PLAYER_TV_STANDARD", "1") == 0);
    assert(s.player_composite == 1 && s.player_tv_standard == 1);

    /* out-of-range rejected */
    assert(settings_set_field_str(&s, "PLAYER_COMPOSITE", "2") != 0);
    assert(settings_set_field_str(&s, "PLAYER_TV_STANDARD", "5") != 0);

    /* get-field mirrors set */
    char buf[SETTINGS_FIELD_STR_MAX];
    assert(settings_get_field_str(&s, "PLAYER_COMPOSITE", buf, sizeof buf) == 0);
    assert(strcmp(buf, "1") == 0);

    /* file round-trip */
    const char *path = "/tmp/pivid_test_composite.conf";
    assert(settings_save_to_file(&s, path) == 0);
    Settings r; settings_load_from_file(&r, path);
    assert(r.player_composite == 1 && r.player_tv_standard == 1);
}

int main(void) {
    test_settings_round_trip();
    test_rf_freq_snaps_to_calib_grid();
    test_defaults_match_validated_recipe();
    test_fec_m_rejects_zero();
    test_crc_width_rejects_off();
    test_rx_boost_range();
    test_settings_defaults_direct();
    test_settings_atomic_no_tempfile();
    test_settings_save_error_path();
    test_settings_long_phrase_truncates();
    test_settings_defaults_when_missing();
    test_settings_get_sync_word();
    test_settings_parse_robustness();
    test_settings_rejects_malformed();
    test_settings_osd_keys();
    test_settings_aqm();
    test_composite_roundtrip();
    printf("All settings tests passed!\n");
    return 0;
}
