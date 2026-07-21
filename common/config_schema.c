#include "config_schema.h"
#include <string.h>

/* ---- enum option tables (NULL-terminated) ---- */
static const EnumOpt opt_flrc_cr[]    = {
    { "1", "3/4 (chip FEC, default)" }, { "2", "1/1 (no chip FEC)" }, { NULL, NULL } };
static const EnumOpt opt_flrc_crc[]   = {
    { "1", "16-bit (default)" }, { "2", "24-bit" }, { "3", "32-bit" },
    { NULL, NULL } };
/* Only the useful upper half of the raw 0..7 field: 0-3 sit at or below the
 * HF reference gain (0 = LF-path default) and were dropped from the UI
 * (user, 2026-07-06). The kernel param still accepts 0..7 for bench use. */
static const EnumOpt opt_rx_boost[]   = {
    { "4", "4 — HF default, non-boosted (recommended)" },
    { "5", "5 — intermediate" },
    { "6", "6 — intermediate" },
    { "7", "7 — max boost (~+0.5 dB sensitivity)" },
    { NULL, NULL } };
static const EnumOpt opt_recipe[]     = {
    { "droppable", "droppable-P (1-frame glitch on loss)" },
    { "gdr", "GDR / intra-refresh (no IDR; software-decode only)" },
    { "gdr_idr", "GDR + periodic IDR (HW-decode safe, default)" },
    { "gop", "Standard GOP (IDR every intra; HW-decode fallback)" },
    { NULL, NULL } };
static const EnumOpt opt_on_off[]     = {
    { "0", "off" }, { "1", "on" }, { NULL, NULL } };
static const EnumOpt opt_tv_standard[] = {
    { "0", "PAL (720x576, 50 Hz)" }, { "1", "NTSC (720x480, 60 Hz)" }, { NULL, NULL } };
static const EnumOpt opt_aqm[]        = {
    { "0", "off" },
    { "enforce", "on (drop whole frames when the link congests)" },
    { NULL, NULL } };

const ConfigKeyMeta config_schema[] = {
  /* key, section, tier, role, restart_unit, type, ui_class, label, description, enum_opts, min, max */
  { "BINDING_PHRASE", "pairing", TIER_RESTART, ROLE_BOTH, NULL,
    FIELD_STR, UI_BASIC, "Binding phrase",
    "Shared secret words (e.g. tall-orange-falcon). Both TX and RX must use the "
    "SAME phrase: it derives the 32-bit sync word that pairs the link. Applying "
    "restarts the daemon to re-pair.", NULL, 0, 0 },

  { "FLRC_CR", "rf", TIER_MODRELOAD, ROLE_BOTH, NULL,
    FIELD_ENUM, UI_ADVANCED, "FLRC coding rate",
    "Chip-side forward error correction. 3/4 is the robust choice; 1/1 trades "
    "robustness for ~20% more throughput. MUST match the other end.",
    opt_flrc_cr, 0, 0 },
  { "FLRC_CRC_WIDTH", "rf", TIER_MODRELOAD, ROLE_BOTH, NULL,
    FIELD_ENUM, UI_ADVANCED, "FLRC CRC width",
    "Chip packet CRC. Wider CRC = exponentially fewer corrupted packets "
    "slipping past as valid (each step is 256x); costs ~0.2%/step airtime. "
    "MUST match the other end.",
    opt_flrc_crc, 0, 0 },
  { "FLRC_PREAMBLE_LEN", "rf", TIER_MODRELOAD, ROLE_BOTH, NULL,
    FIELD_INT, UI_ADVANCED, "FLRC preamble length (code)",
    "agc_pbl_len field code 0..15 (7 = 32-bit preamble). MUST match "
    "the other end.", NULL, 0, 15 },
  { "FLRC_SYNC_LEN", "rf", TIER_MODRELOAD, ROLE_BOTH, NULL,
    FIELD_INT, UI_ADVANCED, "FLRC sync length (code)",
    "sync_len field code 0..3 (2 = 32-bit syncword). MUST match the "
    "other end.", NULL, 0, 3 },
  { "FLRC_PLD_LEN", "rf", TIER_MODRELOAD, ROLE_BOTH, NULL,
    FIELD_INT, UI_ADVANCED, "FLRC payload length",
    "FLRC payload length cap in bytes, 6..511 (511 = max throughput). MUST "
    "match the other end.", NULL, 6, 511 },
  { "FLRC_TX_LOW_THRESH", "rf", TIER_MODRELOAD, ROLE_BOTH, NULL,
    FIELD_INT, UI_ADVANCED, "TX FIFO-low watermark",
    "TX FIFO refill watermark in bytes (0..511). Tuning parameter; "
    "leave it alone unless you know the chip timing.", NULL, 0, 511 },
  { "RF_FREQUENCY", "rf", TIER_MODRELOAD, ROLE_BOTH, NULL,
    FIELD_INT, UI_BASIC, "RF frequency (MHz)",
    "Carrier frequency in MHz. The antenna + PA are matched for the S-band "
    "(~1900-2500 MHz). MUST match the other end. Snapped down to the nearest "
    "4 MHz step (the chip's front-end-calibration grid), so the saved value may "
    "read slightly lower than entered. Applying reloads the radio module (brief "
    "link drop on this unit).",
    NULL, 1900000000L, 2500000000L },
  { "RF_TX_POWER", "rf", TIER_MODRELOAD, ROLE_TX, NULL,
    FIELD_INT, UI_ADVANCED, "TX power (dBm)",
    "Transmit power in dBm. The chip HF PA tops out at +12 dBm, but the default "
    "-3 drives an external SZM-2166Y power amp (~36 dB gain) to ~2 W at 2300 "
    "MHz; do not raise it without bench-measuring the amp output (input "
    "compresses near -1 dBm). Per-unit -- the ends need not match. Applying "
    "reloads the radio "
    "module. (Not shown on the RX portal; the RX never transmits on this "
    "one-way link.)", NULL, -18, 12 },
  { "RX_BOOST", "rf", TIER_MODRELOAD, ROLE_RX, NULL,
    FIELD_ENUM, UI_ADVANCED, "Rx front-end boost",
    "Rx front-end gain boost (LR2021 SetRxPath, datasheet 7.3.2). Receive-side "
    "only: the ends do NOT need to match and it has no effect on a transmitter. "
    "4 is the proven non-boosted default; 7 buys ~0.5 dB sensitivity. Applying "
    "reloads the radio module (brief link drop on this unit).",
    opt_rx_boost, 0, 0 },

  { "WIDTH", "camera", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "Width (px)",
    "Camera capture width in pixels. Applying restarts the encoder "
    "(~1-2 s stream blip).", NULL, 16, 4096 },
  { "HEIGHT", "camera", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "Height (px)",
    "Camera capture height in pixels. Applying restarts the "
    "encoder.", NULL, 16, 4096 },
  { "FPS", "camera", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "Frame rate (fps)",
    "Capture frame rate. Applying restarts the encoder.",
    NULL, 1, 120 },

  { "ENCODER_RECIPE", "encoder", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_ENUM, UI_ADVANCED, "Encoder recipe",
    "Loss-resilience strategy. droppable-P glitches one frame on loss; GDR uses "
    "intra-refresh. Applying restarts the encoder.", opt_recipe, 0, 0 },
  { "BITRATE", "encoder", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "Bitrate (bps)",
    "Target H.264 bitrate in bits/sec. Keep below the link "
    "drain or the ring overflows. Applying restarts the encoder.",
    NULL, 1, 20000000 },
  { "INTRA", "encoder", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "Intra period",
    "Keyframe / refresh period in frames. Applying restarts the "
    "encoder.", NULL, 0, 1000 },
  { "SLICE_MB", "encoder", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "Slice size (macroblocks)",
    "Macroblocks per slice; smaller slices localise loss damage. "
    "Applying restarts the encoder.", NULL, 0, 100000 },
  { "DROPPABLE_LEN", "encoder", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "Droppable GOP length",
    "Disposable-P chain length. Applying restarts the encoder.",
    NULL, 0, 64 },
  { "SPS_REINJECT_MS", "encoder", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "SPS reinject (ms)",
    "Re-send SPS/PPS every N ms so a late-joining decoder recovers (0 = off). "
    "Applying restarts the encoder.", NULL, 0, 60000 },
  { "IDR_PERIOD", "encoder", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "IDR period (frames)",
    "For the gdr_idr recipe: force an IDR keyframe every N frames so the RX "
    "hardware decoder can lock (0 = no forced IDR). Applying "
    "restarts the encoder.", NULL, 0, 1000 },

  { "FEC_M_ANCHOR", "fec", TIER_LIVE, ROLE_TX, NULL,
    FIELD_INT, UI_ADVANCED, "FEC parity: anchors (m)",
    "Reed-Solomon parity packets per IDR/SPS/PPS block (the GOP anchors). "
    "Higher = more protection for the frames whose loss blacks a whole GOP. "
    "Min 1 -- FEC is always on, never 0. Applied "
    "live with no stream drop. The receiver reads parity per block off the wire, "
    "so no matching setting is needed on the other end.",
    NULL, 1, 32 },
  { "FEC_M_REF", "fec", TIER_LIVE, ROLE_TX, NULL,
    FIELD_INT, UI_ADVANCED, "FEC parity: ref-P (m)",
    "Reed-Solomon parity packets per reference-P block. Most parity goes on the "
    "anchors; ref-P carries a lighter share. Min 1 "
    "-- FEC is always on for every class, never 0. Applied live.", NULL, 1, 32 },
  { "FEC_K", "fec", TIER_LIVE, ROLE_TX, NULL,
    FIELD_INT, UI_ADVANCED, "FEC block size (k)",
    "Data packets per FEC block, 1..200. Applied live.",
    NULL, 1, 200 },

  { "FORWARD_TO_PC", "player", TIER_CASTER, ROLE_RX, NULL,
    FIELD_ENUM, UI_BASIC, "Forward video to PC",
    "Advertise this receiver on the network (mDNS) and accept remote PC viewers "
    "running pivid-player. Off = only a locally-connected player sees the stream. "
    "Applied live with no stream drop.", opt_on_off, 0, 0 },
  { "PLAYER_LOCAL", "player", TIER_PLAYER, ROLE_RX, NULL,
    FIELD_ENUM, UI_BASIC, "Local HDMI player",
    "Run the video player on a screen plugged directly into this receiver "
    "(no PC needed). On = start it now and on every boot; off = stop it and "
    "keep it off. Requires the player to be installed on this Pi.", opt_on_off, 0, 0 },
  { "PLAYER_COMPOSITE", "player", TIER_BOOTCFG, ROLE_RX, NULL,
    FIELD_ENUM, UI_BASIC, "Composite (analog TV) output",
    "Drive the local display over the analog composite (RCA/CVBS) jack instead "
    "of HDMI. Enabling turns HDMI off (they can't run at once). Requires a reboot "
    "to take effect -- use the Reboot RX button after applying.", opt_on_off, 0, 0 },
  { "PLAYER_TV_STANDARD", "player", TIER_BOOTCFG, ROLE_RX, NULL,
    FIELD_ENUM, UI_BASIC, "Composite TV standard",
    "Analog video standard for the composite output. PAL (Europe) or NTSC "
    "(US / analog FPV gear). Only used when composite output is on. Requires a "
    "reboot to take effect.", opt_tv_standard, 0, 0 },

  { "PIVID_AP_PASSWORD", "network", TIER_NETCONFIG, ROLE_BOTH, NULL,
    FIELD_STR, UI_BASIC, "Config AP password",
    "WPA2 password for the config-mode hotspot (>=8 chars). "
    "Blank = open AP. Takes effect the NEXT time you enter config mode (changing "
    "it does not drop your current connection).", NULL, 0, 0 },

  { "MSP_ENABLED", "msp", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_ENUM, UI_BASIC, "MSP telemetry",
    "Read Betaflight MSP telemetry from the FC UART and multiplex it onto the "
    "link. Applying restarts the transmitter.", opt_on_off, 0, 0 },
  { "MSP_UART_DEVICE", "msp", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_STR, UI_ADVANCED, "MSP UART device",
    "Serial device for the FC MSP link. Applying restarts "
    "the transmitter.", NULL, 0, 0 },
  { "MSP_BAUD", "msp", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "MSP baud",
    "UART baud for MSP; match the flight controller (Betaflight uses 115200). "
    "Applying restarts the transmitter.", NULL, 9600, 921600 },

  { "AQM", "aqm", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_ENUM, UI_BASIC, "Adaptive frame-drop (AQM)",
    "Proactively drop whole video frames upstream of the radio when the TX ring "
    "backs up, so the receiver skips a clean beat instead of showing mid-frame "
    "corruption. Off = never drop. Applying restarts the transmitter.",
    opt_aqm, 0, 0 },

  { "OSD_KEYFRAME_MS", "osd", TIER_RESTART, ROLE_TX, "pivid-tx",
    FIELD_INT, UI_ADVANCED, "OSD keyframe cadence (ms)",
    "How often the TX sends a full DisplayPort OSD keyframe over the link "
    "in ms. Lower values recover the OSD faster after packet loss "
    "but consume more link bandwidth. Applying restarts the transmitter.",
    NULL, 20, 5000 },
  { "OSD_VID_RSSI_CELL", "osd", TIER_LIVE, ROLE_RX, NULL,
    FIELD_STR, UI_ADVANCED, "Vid RSSI cell (row,col)",
    "Grid cell (row,col) where the RX injects the video-link RSSI dBm value "
    "into the OSD. Row 0..15, col 0..29. Applied live on the next "
    "OSD frame (no RX restart).", NULL, 0, 0 },
  { "OSD_VID_LQ_CELL", "osd", TIER_LIVE, ROLE_RX, NULL,
    FIELD_STR, UI_ADVANCED, "Vid LQ cell (row,col)",
    "Grid cell (row,col) where the RX injects the video-link packet loss quality "
    "value into the OSD. Row 0..15, col 0..29. Applied live on the "
    "next OSD frame (no RX restart).", NULL, 0, 0 },
};

const size_t config_schema_len = sizeof(config_schema) / sizeof(config_schema[0]);

const ConfigKeyMeta *config_schema_lookup(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < config_schema_len; i++)
        if (strcmp(config_schema[i].key, key) == 0) return &config_schema[i];
    return NULL;
}
