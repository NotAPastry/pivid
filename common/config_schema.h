#ifndef COMMON_CONFIG_SCHEMA_H
#define COMMON_CONFIG_SCHEMA_H

#include <stddef.h>

/* Per-key apply tier. The agent (pivid-config-agent) routes each changed key by
 * this; the agent never opens /dev/radio0.
 *   TIER_LIVE      - applied live by the owning daemon on a SIGHUP reload
 *                    (FEC m/k only; verified safe). No restart_unit.
 *   TIER_RESTART   - write pivid.conf, then `systemctl restart` the role daemon
 *                    (camera/encoder/rx + pairing: a restart re-derives the sync
 *                    word at startup). restart_unit documents the unit; the
 *                    engine derives it from the agent role.
 *   TIER_MODRELOAD - chip/driver module params (flrc_*, rf_freq_hz, rf_tx_power):
 *                    write conf -> regen modprobe.d/lr2021.conf -> stop daemon ->
 *                    modprobe -r/modprobe lr2021 -> start daemon. The ONLY safe
 *                    way to change flrc live (a live SET_FLRC_CONFIG wedges the
 *                    chip).
 *   TIER_NETCONFIG - hotspot/network config consumed by pivid-config-net.sh at
 *                    the next config-mode entry (ap_password). conf write only;
 *                    no radio/daemon action.
 *   TIER_CASTER    - applied live by pivid-caster on a SIGHUP reload (the
 *                    "Forward to PC" toggle). Routed INDEPENDENTLY of the single
 *                    radio action: the caster never opens /dev/radio0, so a
 *                    caster reload may run alongside a radio restart/modreload.
 *                    No restart_unit (the target unit is always pivid-caster).
 *   TIER_PLAYER    - enable/disable + start/stop pivid-player.service on the RX
 *                    (`systemctl enable/disable --now`), value-aware. Routed
 *                    INDEPENDENTLY of the single radio action: the player never
 *                    opens /dev/radio0. No restart_unit (target is always
 *                    pivid-player). RX-only.
 *   TIER_BOOTCFG   - boot-firmware setting written to config.txt by a helper
 *                    (pivid-display.sh); requires an OS reboot to take effect.
 *                    Routed INDEPENDENTLY of the radio action (the helper never
 *                    opens /dev/radio0). RX-only. No restart_unit. */
typedef enum { TIER_LIVE, TIER_RESTART, TIER_MODRELOAD, TIER_NETCONFIG, TIER_CASTER, TIER_PLAYER, TIER_BOOTCFG } ApplyTier;

/* Which role(s) surface the key (UI gating). */
typedef enum { ROLE_BOTH, ROLE_TX, ROLE_RX } ConfigRole;

/* Form field type (drives the rendered widget + JSON `type`). */
typedef enum { FIELD_STR, FIELD_INT, FIELD_ENUM } FieldType;

/* Basic view shows UI_BASIC only; an Advanced toggle reveals everything. */
typedef enum { UI_BASIC, UI_ADVANCED } UiClass;

/* One option for a FIELD_ENUM key (rendered as <option value=value>label). */
typedef struct { const char *value; const char *label; } EnumOpt;

typedef struct {
    const char    *key;          /* pivid.conf key, e.g. "BITRATE" */
    const char    *section;      /* pairing/rf/camera/encoder/fec/rx/network */
    ApplyTier      tier;
    ConfigRole     role;
    const char    *restart_unit; /* documents the TIER_RESTART unit; else NULL */
    FieldType      type;
    UiClass        ui_class;
    const char    *label;        /* human label */
    const char    *description;  /* per-key help text */
    const EnumOpt *enum_opts;    /* FIELD_ENUM: NULL-terminated by {NULL,NULL}; else NULL */
    long           min, max;     /* FIELD_INT inclusive range; else 0,0 */
} ConfigKeyMeta;

extern const ConfigKeyMeta config_schema[];
extern const size_t        config_schema_len;

/* Returns the metadata for `key`, or NULL if the key is not in the schema. */
const ConfigKeyMeta *config_schema_lookup(const char *key);

#endif
