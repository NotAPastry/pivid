#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "config_schema.h"
#include "phrase_sync.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Snap an RF carrier down to the LR2021 4 MHz FE-cal grid (see settings.h).
 * Truncate-down matches the chip's no-arg CalibFE slot convention exactly, so
 * the carrier ends up co-located with its front-end calibration slot. The
 * min config bound (1.9 GHz) is itself a 4 MHz multiple, so snapping never
 * drops below the valid range. */
uint64_t snap_rf_freq_to_calib_grid(uint64_t hz) {
    return (hz / LR2021_FE_CALIB_GRID_HZ) * LR2021_FE_CALIB_GRID_HZ;
}

/* ---- defaults: the Phase-E-VALIDATED 480p droppable prod recipe. The recipe-
 * dependent encoder/FEC fields MUST equal recipe_defaults["droppable"]
 * (common/recipe_defaults.c) -- the dashboard Reset button writes these, so a
 * drift silently reintroduces a config the validation rejected. FEC m_anchor and
 * m_ref are >=1 ("FEC always on" -- never 0; see recipe_defaults.c constraint).
 * A drift-guard test (tests/common/test_settings.c) fails CI if these diverge. */
static void settings_apply_defaults(Settings *s) {
    memset(s, 0, sizeof(*s));
    strncpy(s->binding_phrase, DEFAULT_BINDING_PHRASE, SETTINGS_PHRASE_MAX - 1);

    s->flrc_cr = 1; s->flrc_crc_width = 1;
    s->flrc_preamble_len = 7; s->flrc_sync_len = 2;
    s->flrc_pld_len = 511; s->flrc_tx_low_thresh = 192;
    s->rf_frequency = snap_rf_freq_to_calib_grid(2100000000ull); /* 2100 MHz (on the 4 MHz FE-cal grid: 2100/4 = 525) */
    s->rf_tx_power = -3;   /* drives external SZM-2166Y PA to ~2 W (rated max); see lr2021_core.c */
    s->rx_boost = 4;   /* SetRxPath boost: non-boosted HF default (docs/DECISIONS.md) */

    s->width = 640; s->height = 480; s->fps = 30;

    s->bitrate = 850000;
    strncpy(s->recipe, "droppable", SETTINGS_RECIPE_MAX - 1);
    s->intra = 30; s->slice_mb = 120; s->droppable_len = 8; s->sps_reinject_ms = 0;
    s->idr_period = 60;
    strncpy(s->aqm, "enforce", SETTINGS_AQM_MAX - 1);

    s->fec_m_anchor = 9; s->fec_m_ref = 2; s->fec_k = 16;

    strncpy(s->ap_password, "broadcast", SETTINGS_AP_PW_MAX - 1);

    s->msp_enabled = 1;          /* TX default: read FC MSP telemetry (RX ignores) */
    strncpy(s->msp_uart_device, "/dev/ttyAMA0", SETTINGS_UART_MAX - 1);
    s->msp_baud = 115200;

    s->osd_keyframe_ms = 200;
    s->osd_vid_rssi_row = 1; s->osd_vid_rssi_col = 1;   /* 1 row from top, 1 col from left */
    s->osd_vid_lq_row = 2;   s->osd_vid_lq_col = 1;     /* stacked one row below RSSI */

    s->forward_to_pc = 0;
    s->player_local = 0;
    s->player_composite = 0;     /* HDMI by default */
    s->player_tv_standard = 0;   /* PAL */
}

void settings_defaults(Settings *s) { settings_apply_defaults(s); }

/* Copy a value into dst: null-terminate, strip trailing CR/LF, then strip one
 * matching pair of surrounding double quotes (we serialize strings quoted so
 * the file stays shell-sourceable). */
static void copy_value(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    /* Measure src up to the first CR/LF, strip one surrounding quote pair on
     * the FULL value, THEN truncate to the buffer. Quoting before truncation
     * means a value near the cap can't lose only its closing quote to the
     * truncation and keep a stray opening quote. */
    size_t slen = strcspn(src, "\r\n");
    if (slen >= 2 && src[0] == '"' && src[slen - 1] == '"') {
        src += 1;
        slen -= 2;
    }
    size_t copy = slen < dst_size - 1 ? slen : dst_size - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
}

/* Parse an int key; leave *out untouched (default preserved) on malformed or
 * out-of-range input. lo/hi are inclusive bounds. Base 10 (not 0): these are
 * decimal tunables, so a hand-edited leading zero must NOT be read as octal. */
/* True once *p is the string terminator, skipping only trailing whitespace
 * (the file loader feeds raw fgets() lines that still carry a '\n'). This
 * still rejects real garbage like "12abc". */
static int only_trailing_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return *p == '\0';
}

/* returns 1 on success (value applied), 0 on malformed/out-of-range (unchanged) */
static int parse_int(const char *val, int lo, int hi, int *out, const char *key) {
    char *end = NULL;
    errno = 0;
    long v = strtol(val, &end, 10);
    if (errno == 0 && end != val && only_trailing_ws(end) && v >= lo && v <= hi) {
        *out = (int)v;
        return 1;
    }
    fprintf(stderr, "settings: rejecting malformed/out-of-range %s\n", key);
    return 0;
}

/* set one int field; returns 1 on success, 0 on malformed/out-of-range. */
static int set_int(const char *val, int lo, int hi, int *out, const char *key) {
    return parse_int(val, lo, hi, out, key);
}

/* Parse a "row,col" string; row 0..15 (OSD_GRID_ROWS-1), col 0..29
 * (OSD_GRID_COLS-1). Returns 1 on success, 0 on malformed/out-of-range. */
static int set_cell(const char *val, int *row, int *col, const char *key) {
    int r = 0, c = 0;
    if (sscanf(val, "%d,%d", &r, &c) == 2 && r >= 0 && r <= 15 && c >= 0 && c <= 29) {
        *row = r; *col = c;
        return 1;
    }
    fprintf(stderr, "settings: rejecting malformed/out-of-range %s (expects row 0..15,col 0..29)\n", key);
    return 0;
}

/* set a uint64 field; returns 1 on success, 0 on malformed/out-of-range. */
static int set_u64(const char *val, unsigned long long lo, unsigned long long hi,
                   uint64_t *out, const char *key) {
    char *end = NULL; errno = 0;
    unsigned long long v = strtoull(val, &end, 10);
    if (errno == 0 && end != val && only_trailing_ws(end) && v >= lo && v <= hi) {
        *out = (uint64_t)v;
        return 1;
    }
    fprintf(stderr, "settings: rejecting malformed/out-of-range %s\n", key);
    return 0;
}

int settings_get_field_str(const Settings *s, const char *key, char *buf, size_t cap) {
    if (!s || !key || !buf || cap == 0) return -1;
    #define G_INT(K, F)  if (strcmp(key, K) == 0) { snprintf(buf, cap, "%d", (F)); return 0; }
    #define G_STR(K, F)  if (strcmp(key, K) == 0) { snprintf(buf, cap, "%s", (F)); return 0; }
    G_STR("BINDING_PHRASE",   s->binding_phrase)
    G_INT("FLRC_CR",          s->flrc_cr)
    G_INT("FLRC_CRC_WIDTH",   s->flrc_crc_width)
    G_INT("FLRC_PREAMBLE_LEN",s->flrc_preamble_len)
    G_INT("FLRC_SYNC_LEN",    s->flrc_sync_len)
    G_INT("FLRC_PLD_LEN",     s->flrc_pld_len)
    G_INT("FLRC_TX_LOW_THRESH",s->flrc_tx_low_thresh)
    if (strcmp(key, "RF_FREQUENCY") == 0) { snprintf(buf, cap, "%llu", (unsigned long long)s->rf_frequency); return 0; }
    G_INT("RF_TX_POWER",      s->rf_tx_power)
    G_INT("RX_BOOST",         s->rx_boost)
    G_INT("WIDTH",            s->width)
    G_INT("HEIGHT",           s->height)
    G_INT("FPS",              s->fps)
    G_INT("BITRATE",          s->bitrate)
    G_STR("ENCODER_RECIPE",   s->recipe)
    G_STR("AQM",             s->aqm)
    G_INT("INTRA",            s->intra)
    G_INT("SLICE_MB",         s->slice_mb)
    G_INT("DROPPABLE_LEN",    s->droppable_len)
    G_INT("SPS_REINJECT_MS",  s->sps_reinject_ms)
    G_INT("IDR_PERIOD",       s->idr_period)
    G_INT("FEC_M_ANCHOR",     s->fec_m_anchor)
    G_INT("FEC_M_REF",        s->fec_m_ref)
    G_INT("FEC_K",            s->fec_k)
    G_STR("PIVID_AP_PASSWORD",      s->ap_password)
    G_INT("MSP_ENABLED",      s->msp_enabled)
    G_STR("MSP_UART_DEVICE",  s->msp_uart_device)
    G_INT("MSP_BAUD",         s->msp_baud)
    G_INT("OSD_KEYFRAME_MS",  s->osd_keyframe_ms)
    if (strcmp(key, "OSD_VID_RSSI_CELL") == 0) { snprintf(buf, cap, "%d,%d", s->osd_vid_rssi_row, s->osd_vid_rssi_col); return 0; }
    if (strcmp(key, "OSD_VID_LQ_CELL") == 0)   { snprintf(buf, cap, "%d,%d", s->osd_vid_lq_row, s->osd_vid_lq_col); return 0; }
    G_INT("FORWARD_TO_PC",    s->forward_to_pc)
    G_INT("PLAYER_LOCAL",     s->player_local)
    G_INT("PLAYER_COMPOSITE",    s->player_composite)
    G_INT("PLAYER_TV_STANDARD",  s->player_tv_standard)
    #undef G_INT
    #undef G_STR
    return -1;
}

int settings_set_field_str(Settings *s, const char *key, const char *val) {
    if (!s || !key || !val) return -1;
    if (strcmp(key, "BINDING_PHRASE") == 0)      { copy_value(s->binding_phrase, SETTINGS_PHRASE_MAX, val); return 0; }
    if (strcmp(key, "FLRC_CR") == 0)             { return set_int(val, 0, 15, &s->flrc_cr, key) ? 0 : -1; }
    if (strcmp(key, "FLRC_CRC_WIDTH") == 0)      { return set_int(val, 1, 3, &s->flrc_crc_width, key) ? 0 : -1; }
    if (strcmp(key, "FLRC_PREAMBLE_LEN") == 0)   { return set_int(val, 0, 15, &s->flrc_preamble_len, key) ? 0 : -1; }
    if (strcmp(key, "FLRC_SYNC_LEN") == 0)       { return set_int(val, 0, 3, &s->flrc_sync_len, key) ? 0 : -1; }
    if (strcmp(key, "FLRC_PLD_LEN") == 0)        { return set_int(val, 6, 511, &s->flrc_pld_len, key) ? 0 : -1; }
    if (strcmp(key, "FLRC_TX_LOW_THRESH") == 0)  { return set_int(val, 0, 511, &s->flrc_tx_low_thresh, key) ? 0 : -1; }
    if (strcmp(key, "WIDTH") == 0)               { return set_int(val, 16, 4096, &s->width, key) ? 0 : -1; }
    if (strcmp(key, "HEIGHT") == 0)              { return set_int(val, 16, 4096, &s->height, key) ? 0 : -1; }
    if (strcmp(key, "FPS") == 0)                 { return set_int(val, 1, 120, &s->fps, key) ? 0 : -1; }
    if (strcmp(key, "BITRATE") == 0)             { return set_int(val, 1, 2147483647, &s->bitrate, key) ? 0 : -1; }
    if (strcmp(key, "ENCODER_RECIPE") == 0)      { copy_value(s->recipe, SETTINGS_RECIPE_MAX, val); return 0; }
    if (strcmp(key, "AQM") == 0)                { copy_value(s->aqm, SETTINGS_AQM_MAX, val); return 0; }
    if (strcmp(key, "INTRA") == 0)               { return set_int(val, 0, 1000, &s->intra, key) ? 0 : -1; }
    if (strcmp(key, "SLICE_MB") == 0)            { return set_int(val, 0, 100000, &s->slice_mb, key) ? 0 : -1; }
    if (strcmp(key, "DROPPABLE_LEN") == 0)       { return set_int(val, 0, 64, &s->droppable_len, key) ? 0 : -1; }
    if (strcmp(key, "SPS_REINJECT_MS") == 0)     { return set_int(val, 0, 60000, &s->sps_reinject_ms, key) ? 0 : -1; }
    if (strcmp(key, "IDR_PERIOD") == 0)          { return set_int(val, 0, 1000, &s->idr_period, key) ? 0 : -1; }
    if (strcmp(key, "FEC_M_ANCHOR") == 0)        { return set_int(val, 1, 32, &s->fec_m_anchor, key) ? 0 : -1; }
    if (strcmp(key, "FEC_M_REF") == 0)           { return set_int(val, 1, 32, &s->fec_m_ref, key) ? 0 : -1; }
    if (strcmp(key, "FEC_K") == 0)               { return set_int(val, 1, 200, &s->fec_k, key) ? 0 : -1; }
    if (strcmp(key, "RF_FREQUENCY") == 0)        { if (!set_u64(val, 1900000000ull, 2500000000ull, &s->rf_frequency, key)) return -1;
                                                   s->rf_frequency = snap_rf_freq_to_calib_grid(s->rf_frequency); return 0; }
    if (strcmp(key, "RF_TX_POWER") == 0)         { return set_int(val, -18, 12, &s->rf_tx_power, key) ? 0 : -1; }
    if (strcmp(key, "RX_BOOST") == 0)            { return set_int(val, 4, 7, &s->rx_boost, key) ? 0 : -1; }
    if (strcmp(key, "PIVID_AP_PASSWORD") == 0)         { copy_value(s->ap_password, SETTINGS_AP_PW_MAX, val); return 0; }
    if (strcmp(key, "MSP_ENABLED") == 0)         { return set_int(val, 0, 1, &s->msp_enabled, key) ? 0 : -1; }
    if (strcmp(key, "MSP_UART_DEVICE") == 0)     { copy_value(s->msp_uart_device, SETTINGS_UART_MAX, val); return 0; }
    if (strcmp(key, "MSP_BAUD") == 0)            { return set_int(val, 9600, 921600, &s->msp_baud, key) ? 0 : -1; }
    if (strcmp(key, "OSD_KEYFRAME_MS") == 0)     { return set_int(val, 20, 5000, &s->osd_keyframe_ms, key) ? 0 : -1; }
    if (strcmp(key, "OSD_VID_RSSI_CELL") == 0)   { return set_cell(val, &s->osd_vid_rssi_row, &s->osd_vid_rssi_col, key) ? 0 : -1; }
    if (strcmp(key, "OSD_VID_LQ_CELL") == 0)     { return set_cell(val, &s->osd_vid_lq_row, &s->osd_vid_lq_col, key) ? 0 : -1; }
    if (strcmp(key, "FORWARD_TO_PC") == 0)       { return set_int(val, 0, 1, &s->forward_to_pc, key) ? 0 : -1; }
    if (strcmp(key, "PLAYER_LOCAL") == 0)        { return set_int(val, 0, 1, &s->player_local, key) ? 0 : -1; }
    if (strcmp(key, "PLAYER_COMPOSITE") == 0)    { return set_int(val, 0, 1, &s->player_composite, key) ? 0 : -1; }
    if (strcmp(key, "PLAYER_TV_STANDARD") == 0)  { return set_int(val, 0, 1, &s->player_tv_standard, key) ? 0 : -1; }
    return -1;
}

/* "FOO=" prefix test; returns the value pointer past '=' or NULL. */
static const char *match(const char *line, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) == 0 && line[klen] == '=') return line + klen + 1;
    return NULL;
}

int settings_load_from_file(Settings *s, const char *path) {
    if (!s || !path) return -1;
    settings_apply_defaults(s);

    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        const char *v;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
            if      ((v = match(line, "BINDING_PHRASE")))   copy_value(s->binding_phrase, SETTINGS_PHRASE_MAX, v);
            else if ((v = match(line, "FLRC_CR")))          parse_int(v, 0, 15, &s->flrc_cr, "FLRC_CR");
            else if ((v = match(line, "FLRC_CRC_WIDTH")))   parse_int(v, 1, 3, &s->flrc_crc_width, "FLRC_CRC_WIDTH");
            else if ((v = match(line, "FLRC_PREAMBLE_LEN"))) parse_int(v, 0, 15, &s->flrc_preamble_len, "FLRC_PREAMBLE_LEN");
            else if ((v = match(line, "FLRC_SYNC_LEN")))    parse_int(v, 0, 3, &s->flrc_sync_len, "FLRC_SYNC_LEN");
            else if ((v = match(line, "FLRC_PLD_LEN")))     parse_int(v, 6, 511, &s->flrc_pld_len, "FLRC_PLD_LEN");
            else if ((v = match(line, "FLRC_TX_LOW_THRESH"))) parse_int(v, 0, 511, &s->flrc_tx_low_thresh, "FLRC_TX_LOW_THRESH");
            else if ((v = match(line, "RF_FREQUENCY")))  { set_u64(v, 1900000000ull, 2500000000ull, &s->rf_frequency, "RF_FREQUENCY");
                                                           s->rf_frequency = snap_rf_freq_to_calib_grid(s->rf_frequency); }
            else if ((v = match(line, "RF_TX_POWER")))      parse_int(v, -18, 12, &s->rf_tx_power, "RF_TX_POWER");
            else if ((v = match(line, "RX_BOOST")))         parse_int(v, 4, 7, &s->rx_boost, "RX_BOOST");
            else if ((v = match(line, "WIDTH")))            parse_int(v, 16, 4096, &s->width, "WIDTH");
            else if ((v = match(line, "HEIGHT")))           parse_int(v, 16, 4096, &s->height, "HEIGHT");
            else if ((v = match(line, "FPS")))              parse_int(v, 1, 120, &s->fps, "FPS");
            else if ((v = match(line, "BITRATE")))          parse_int(v, 1, INT_MAX, &s->bitrate, "BITRATE");
            else if ((v = match(line, "ENCODER_RECIPE")))   copy_value(s->recipe, SETTINGS_RECIPE_MAX, v);
            else if ((v = match(line, "AQM")))              copy_value(s->aqm, SETTINGS_AQM_MAX, v);
            else if ((v = match(line, "INTRA")))            parse_int(v, 0, 1000, &s->intra, "INTRA");
            else if ((v = match(line, "SLICE_MB")))         parse_int(v, 0, 100000, &s->slice_mb, "SLICE_MB");
            else if ((v = match(line, "DROPPABLE_LEN")))    parse_int(v, 0, 64, &s->droppable_len, "DROPPABLE_LEN");
            else if ((v = match(line, "SPS_REINJECT_MS")))  parse_int(v, 0, 60000, &s->sps_reinject_ms, "SPS_REINJECT_MS");
            else if ((v = match(line, "IDR_PERIOD")))       parse_int(v, 0, 1000, &s->idr_period, "IDR_PERIOD");
            else if ((v = match(line, "FEC_M_ANCHOR")))     parse_int(v, 1, 32, &s->fec_m_anchor, "FEC_M_ANCHOR");
            else if ((v = match(line, "FEC_M_REF")))        parse_int(v, 1, 32, &s->fec_m_ref, "FEC_M_REF");
            else if ((v = match(line, "FEC_K")))            parse_int(v, 1, 200, &s->fec_k, "FEC_K");
            else if ((v = match(line, "PIVID_AP_PASSWORD")))      copy_value(s->ap_password, SETTINGS_AP_PW_MAX, v);
            else if ((v = match(line, "MSP_ENABLED")))      parse_int(v, 0, 1, &s->msp_enabled, "MSP_ENABLED");
            else if ((v = match(line, "MSP_UART_DEVICE")))  copy_value(s->msp_uart_device, SETTINGS_UART_MAX, v);
            else if ((v = match(line, "MSP_BAUD")))         parse_int(v, 9600, 921600, &s->msp_baud, "MSP_BAUD");
            else if ((v = match(line, "OSD_KEYFRAME_MS")))  parse_int(v, 20, 5000, &s->osd_keyframe_ms, "OSD_KEYFRAME_MS");
            else if ((v = match(line, "OSD_VID_RSSI_CELL"))) set_cell(v, &s->osd_vid_rssi_row, &s->osd_vid_rssi_col, "OSD_VID_RSSI_CELL");
            else if ((v = match(line, "OSD_VID_LQ_CELL")))  set_cell(v, &s->osd_vid_lq_row, &s->osd_vid_lq_col, "OSD_VID_LQ_CELL");
            else if ((v = match(line, "FORWARD_TO_PC"))) parse_int(v, 0, 1, &s->forward_to_pc, "FORWARD_TO_PC");
            else if ((v = match(line, "PLAYER_LOCAL"))) parse_int(v, 0, 1, &s->player_local, "PLAYER_LOCAL");
            else if ((v = match(line, "PLAYER_COMPOSITE")))   parse_int(v, 0, 1, &s->player_composite, "PLAYER_COMPOSITE");
            else if ((v = match(line, "PLAYER_TV_STANDARD"))) parse_int(v, 0, 1, &s->player_tv_standard, "PLAYER_TV_STANDARD");
        }
        fclose(f);
    }
    /* Missing file is not an error: caller gets the defaults above. */

    s->sync_word = phrase_to_sync_word(s->binding_phrase);
    return 0;
}

/* Extract KEY and VALUE from a "KEY=value" line into caller buffers. Returns 1
 * if a key=value pair was extracted, 0 for blank/comment/no '='. VALUE is the
 * raw text after '=' (quotes/newline left for settings_set_field_str to strip,
 * exactly as the file loader's match() pointer would). Used only by
 * settings_load_checked. */
static int parse_line(const char *line, char *key, size_t ksz, char *val, size_t vsz) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') return 0;
    const char *eq = strchr(line, '=');
    if (!eq || eq == line) return 0;
    size_t klen = (size_t)(eq - line);
    if (klen + 1 > ksz) return 0;
    memcpy(key, line, klen);
    key[klen] = '\0';
    snprintf(val, vsz, "%s", eq + 1);
    return 1;
}

int settings_load_checked(Settings *s, const char *path, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!s || !path) { if (err && errcap) snprintf(err, errcap, "null arg"); return -1; }
    settings_apply_defaults(s);

    FILE *f = fopen(path, "r");
    if (!f) { if (err && errcap) snprintf(err, errcap, "cannot open %s", path); return -1; }

    char line[256];
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[256];
        if (!parse_line(line, key, sizeof key, val, sizeof val)) continue;
        if (settings_set_field_str(s, key, val) == -1) {
            char probe[SETTINGS_FIELD_STR_MAX];
            if (settings_get_field_str(s, key, probe, sizeof probe) == 0) {
                /* known key, bad value -> fail the load */
                if (err && errcap) snprintf(err, errcap, "invalid value for %s", key);
                rc = -1;
                break;
            }
            /* unknown key -> tolerate, continue (like the file loader) */
        }
    }
    fclose(f);

    if (rc == 0) s->sync_word = phrase_to_sync_word(s->binding_phrase);
    return rc;
}

int settings_validate(const Settings *s, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!s) { if (err && errcap) snprintf(err, errcap, "null settings"); return -1; }

    if (strcmp(s->recipe, "droppable") && strcmp(s->recipe, "gdr") &&
        strcmp(s->recipe, "gdr_idr") && strcmp(s->recipe, "gop")) {
        if (err && errcap) snprintf(err, errcap, "ENCODER_RECIPE: unknown recipe '%s'", s->recipe);
        return -1;
    }
    if (strcmp(s->aqm, "0") && strcmp(s->aqm, "enforce")) {
        if (err && errcap) snprintf(err, errcap, "AQM: must be 0 or enforce");
        return -1;
    }
    if (s->fec_k < 1) {
        if (err && errcap) snprintf(err, errcap, "FEC_K: must be >= 1");
        return -1;
    }
    return 0;
}

int settings_check_file(const char *path, char *err, size_t errcap) {
    Settings s;
    if (settings_load_checked(&s, path, err, errcap) != 0) return -1;
    return settings_validate(&s, err, errcap);
}

int settings_save_to_file(Settings *s, const char *path) {
    if (!s || !path) return -1;

    /* Atomic, power-safe write (spec §8): write temp -> fsync(temp) ->
     * rename -> fsync(dir). The temp lives in the target's directory so the
     * rename is on one filesystem. */
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmpXXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;

    int fd = mkstemp(tmp);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }

    fprintf(f, "# /opt/pivid/pivid.conf -- canonical PiVid config (single source of truth).\n");
    fprintf(f, "# Shell-sourceable KEY=value; '#' comments. Written atomically by the\n");
    fprintf(f, "# config web UI; parsed by every daemon via common/settings.c.\n");
    fprintf(f, "\n# --- [pairing] ---\n");
    fprintf(f, "BINDING_PHRASE=\"%s\"\n", s->binding_phrase);
    fprintf(f, "\n# --- [rf] --- (FLRC: Live tier; TX and RX MUST match)\n");
    fprintf(f, "FLRC_CR=%d\n", s->flrc_cr);
    fprintf(f, "FLRC_CRC_WIDTH=%d\n", s->flrc_crc_width);
    fprintf(f, "FLRC_PREAMBLE_LEN=%d\n", s->flrc_preamble_len);
    fprintf(f, "FLRC_SYNC_LEN=%d\n", s->flrc_sync_len);
    fprintf(f, "FLRC_PLD_LEN=%d\n", s->flrc_pld_len);
    fprintf(f, "FLRC_TX_LOW_THRESH=%d\n", s->flrc_tx_low_thresh);
    fprintf(f, "# editable via TIER_MODRELOAD (lr2021 module params)\n");
    fprintf(f, "RF_FREQUENCY=%llu\n", (unsigned long long)s->rf_frequency);
    fprintf(f, "RF_TX_POWER=%d\n", s->rf_tx_power);
    fprintf(f, "RX_BOOST=%d\n", s->rx_boost);
    fprintf(f, "\n# --- [camera] ---\n");
    fprintf(f, "WIDTH=%d\n", s->width);
    fprintf(f, "HEIGHT=%d\n", s->height);
    fprintf(f, "FPS=%d\n", s->fps);
    fprintf(f, "\n# --- [encoder] ---\n");
    fprintf(f, "BITRATE=%d\n", s->bitrate);
    fprintf(f, "ENCODER_RECIPE=\"%s\"\n", s->recipe);
    fprintf(f, "AQM=\"%s\"\n", s->aqm);
    fprintf(f, "INTRA=%d\n", s->intra);
    fprintf(f, "SLICE_MB=%d\n", s->slice_mb);
    fprintf(f, "DROPPABLE_LEN=%d\n", s->droppable_len);
    fprintf(f, "SPS_REINJECT_MS=%d\n", s->sps_reinject_ms);
    fprintf(f, "IDR_PERIOD=%d\n", s->idr_period);
    fprintf(f, "\n# --- [fec] ---\n");
    fprintf(f, "FEC_M_ANCHOR=%d\n", s->fec_m_anchor);
    fprintf(f, "FEC_M_REF=%d\n", s->fec_m_ref);
    fprintf(f, "FEC_K=%d\n", s->fec_k);
    fprintf(f, "\n# --- [network] --- (config-mode AP)\n");
    fprintf(f, "PIVID_AP_PASSWORD=\"%s\"\n", s->ap_password);
    fprintf(f, "\n# --- [msp] --- (MSP telemetry; TX role)\n");
    fprintf(f, "MSP_ENABLED=%d\n", s->msp_enabled);
    fprintf(f, "MSP_UART_DEVICE=\"%s\"\n", s->msp_uart_device);
    fprintf(f, "MSP_BAUD=%d\n", s->msp_baud);
    fprintf(f, "\n# --- [osd] --- (MSP DisplayPort OSD)\n");
    fprintf(f, "OSD_KEYFRAME_MS=%d\n", s->osd_keyframe_ms);
    fprintf(f, "OSD_VID_RSSI_CELL=%d,%d\n", s->osd_vid_rssi_row, s->osd_vid_rssi_col);
    fprintf(f, "OSD_VID_LQ_CELL=%d,%d\n", s->osd_vid_lq_row, s->osd_vid_lq_col);
    fprintf(f, "\n# --- [caster] --- (distribution hub; RX role)\n");
    fprintf(f, "FORWARD_TO_PC=%d\n", s->forward_to_pc);
    fprintf(f, "\n# --- [player] --- (local HDMI player; RX role)\n");
    fprintf(f, "PLAYER_LOCAL=%d\n", s->player_local);
    fprintf(f, "PLAYER_COMPOSITE=%d\n", s->player_composite);
    fprintf(f, "PLAYER_TV_STANDARD=%d\n", s->player_tv_standard);

    if (fflush(f) != 0)      { fclose(f); unlink(tmp); return -1; }
    if (fsync(fileno(f)) != 0) { fclose(f); unlink(tmp); return -1; }
    if (fclose(f) != 0)      { unlink(tmp); return -1; }

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

    /* fsync the directory so the rename itself survives a power pull. Derive
     * the dir from path; "." if there is no slash. */
    char dirbuf[PATH_MAX];
    strncpy(dirbuf, path, sizeof(dirbuf) - 1);
    dirbuf[sizeof(dirbuf) - 1] = '\0';
    char *slash = strrchr(dirbuf, '/');
    const char *dir;
    if (!slash) {
        dir = ".";              /* no slash: target is in the cwd            */
    } else if (slash == dirbuf) {
        dir = "/";              /* root-level path like "/foo": parent is "/" */
    } else {
        *slash = '\0';          /* truncate at the last slash -> parent dir   */
        dir = dirbuf;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }

    return 0;
}

int settings_load(Settings *s) { return settings_load_from_file(s, SETTINGS_FILE); }
int settings_save(Settings *s) { return settings_save_to_file(s, SETTINGS_FILE); }

uint32_t settings_get_sync_word(Settings *s) {
    return s ? s->sync_word : DEFAULT_SYNC_WORD;
}

int settings_diff_by_tier(const Settings *cur, const Settings *next, int tier,
                          void (*cb)(const char *key, const char *newval, void *ud),
                          void *ud) {
    int count = 0;
    for (size_t i = 0; i < config_schema_len; i++) {
        const ConfigKeyMeta *m = &config_schema[i];
        if ((int)m->tier != tier) continue;
        char a[SETTINGS_FIELD_STR_MAX], b[SETTINGS_FIELD_STR_MAX];
        if (settings_get_field_str(cur,  m->key, a, sizeof a) != 0) continue;
        if (settings_get_field_str(next, m->key, b, sizeof b) != 0) continue;
        if (strcmp(a, b) == 0) continue;
        count++;
        if (cb) cb(m->key, b, ud);
    }
    return count;
}
