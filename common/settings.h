#ifndef COMMON_SETTINGS_H
#define COMMON_SETTINGS_H

#include <stdint.h>
#include <stddef.h>

/* Canonical config file: single source of truth for ALL tunables. Shell-
 * sourceable KEY=value ('#' comments). Every daemon parses it via this
 * module; it is also sourceable by shell helpers. */
#define SETTINGS_FILE     "/opt/pivid/pivid.conf"
#define DEFAULT_SYNC_WORD 0x12345678   /* NULL-Settings fallback only (see .c) */

/* String field buffer sizes (include the null terminator). */
#define SETTINGS_PHRASE_MAX    64
#define SETTINGS_RECIPE_MAX    16
#define SETTINGS_AQM_MAX       16   /* "0" / "enforce"; dashboard on/off toggle */
#define SETTINGS_AP_PW_MAX     64   /* WPA2 passphrase: 8..63 chars + null */
#define SETTINGS_UART_MAX      64   /* MSP UART device path + null         */
#define SETTINGS_FIELD_STR_MAX 64   /* upper bound for any string field value */

/* LR2021 front-end-calibration frequency grid (DS 6.4.2). The no-arg CalibFE
 * issued at driver bring-up (drivers/lr2021/lr2021_init.c step 7) selects its
 * cal slot by truncating the RF carrier DOWN to a 4 MHz multiple. We snap the
 * configured carrier to this grid on the way in (snap_rf_freq_to_calib_grid) so
 * the carrier sits exactly on its FE-cal slot and the file/dashboard show the
 * frequency the chip actually operates at. See DEEP_RESULTS.md BUG-1. */
#define LR2021_FE_CALIB_GRID_HZ 4000000ull

typedef struct {
    /* [pairing] */
    char     binding_phrase[SETTINGS_PHRASE_MAX];
    uint32_t sync_word;          /* DERIVED from binding_phrase at load;
                                    NOT a file key, not serialized.       */

    /* [rf] FLRC (Live tier) -- raw datasheet field codes (radio_flrc_config) */
    int flrc_cr;
    int flrc_crc_width;
    int flrc_preamble_len;
    int flrc_sync_len;
    int flrc_pld_len;            /* 6..511                                */
    int flrc_tx_low_thresh;
    /* [rf] module-reload tier: settable; clamp [1.9e9,2.5e9] Hz / [-18,12] dBm */
    uint64_t rf_frequency;       /* Hz                                    */
    int      rf_tx_power;        /* dBm                                   */
    int      rx_boost;           /* SetRxPath boost 0..7 (RX-side only)   */

    /* [camera] */
    int width, height, fps;

    /* [encoder] */
    int  bitrate;
    char recipe[SETTINGS_RECIPE_MAX];   /* "droppable" / "gdr" / "gdr_idr" / "gop" */
    int  intra, slice_mb, droppable_len, sps_reinject_ms;
    int  idr_period;             /* IDR cadence (frames) for gdr_idr recipe       */
    char aqm[SETTINGS_AQM_MAX]; /* "0" / "enforce"; dashboard on/off toggle      */

    /* [fec] */
    int fec_m_anchor, fec_m_ref, fec_k;

    /* [network] */
    char ap_password[SETTINGS_AP_PW_MAX];   /* WPA2 PSK for config AP; "" = open */

    /* [msp] MSP telemetry (TX role) */
    int  msp_enabled;            /* 0/1                                   */
    char msp_uart_device[SETTINGS_UART_MAX];
    int  msp_baud;

    /* [osd] MSP DisplayPort OSD */
    int  osd_keyframe_ms;        /* OSD_KEYFRAME_MS: TX keyframe cadence, default 200 */
    int  osd_vid_rssi_row, osd_vid_rssi_col; /* OSD_VID_RSSI_CELL "row,col", default 0,0 */
    int  osd_vid_lq_row, osd_vid_lq_col;    /* OSD_VID_LQ_CELL  "row,col", default 1,0 */

    /* [caster] distribution hub (RX role) */
    int  forward_to_pc;          /* 0/1: advertise mDNS + accept remote subs */

    /* [player] local HDMI player (RX role) */
    int  player_local;           /* 0/1: enable+run pivid-player on the RX HDMI */
    int  player_composite;       /* 0/1: RX local display = composite (CVBS) vs HDMI */
    int  player_tv_standard;     /* 0=PAL, 1=NTSC (only meaningful when composite) */
} Settings;

/* Snap an RF carrier (Hz) DOWN to the LR2021 FE-cal 4 MHz grid
 * (LR2021_FE_CALIB_GRID_HZ), matching the chip's no-arg CalibFE slot selection.
 * Pure; applied to RF_FREQUENCY on every set/load so the stored + displayed
 * value equals the frequency the chip is actually tuned + calibrated for. */
uint64_t snap_rf_freq_to_calib_grid(uint64_t hz);

/* Fill s with the built-in factory defaults (the documented 480p prod recipe). */
void settings_defaults(Settings *s);

/* Load/save against the default SETTINGS_FILE path. settings_load derives
 * sync_word from binding_phrase. */
int settings_load(Settings *s);
int settings_save(Settings *s);

/* Same API, explicit path (used by tests). */
int settings_load_from_file(Settings *s, const char *path);
int settings_save_to_file(Settings *s, const char *path);

/* Validate ranges + cross-field rules (enum membership, etc). Returns 0 if
 * valid, -1 with the first failing field's message in err (NUL-terminated,
 * <= errcap). Opens no device. */
int settings_validate(const Settings *s, char *err, size_t errcap);

/* Load `path`, failing on the first line whose value is rejected by the
 * per-field setter. 0 = ok, -1 with the offending key in err. Missing file
 * is an error. Unknown keys in the file are tolerated (skipped). */
int settings_load_checked(Settings *s, const char *path, char *err, size_t errcap);

/* settings_load_checked + settings_validate. Opens no device. */
int settings_check_file(const char *path, char *err, size_t errcap);

/* Returns the derived sync word; DEFAULT_SYNC_WORD when s is NULL. */
uint32_t settings_get_sync_word(Settings *s);

/* Format the value of schema key `key` from `s` into `buf` (size `cap`) as a
 * string (numbers decimal; strings verbatim, unquoted). Returns 0 on success,
 * -1 if `key` is unknown. */
int settings_get_field_str(const Settings *s, const char *key, char *buf, size_t cap);

/* Parse `val` (a form value, unquoted) into the field for schema key `key`,
 * with the same range clamping as the file parser. Returns 0 on success, -1 if
 * `key` is unknown (out-of-range values leave the field unchanged). */
int settings_set_field_str(Settings *s, const char *key, const char *val);

/* For each schema key with the given tier whose value differs between `cur`
 * and `next`, invoke cb(key, newval, ud). Returns the count of changed keys of
 * that tier. Used by the daemon reload to apply only live-tier diffs and to log
 * pending restart-tier diffs. tier is an ApplyTier (config_schema.h). */
int settings_diff_by_tier(const Settings *cur, const Settings *next, int tier,
                          void (*cb)(const char *key, const char *newval, void *ud),
                          void *ud);

#endif
