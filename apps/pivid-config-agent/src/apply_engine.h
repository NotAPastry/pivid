#ifndef AGENT_APPLY_ENGINE_H
#define AGENT_APPLY_ENGINE_H
#include <stddef.h>
#include "agent.h"
#include "settings.h"

/* Injected side effects so host tests can mock them. Each returns 0/-1. */
typedef struct {
    int (*write_conf)(void *ctx, const Settings *s);     /* atomic pivid.conf   */
    int (*regen_modprobe)(void *ctx, const Settings *s); /* lr2021.conf params   */
    int (*reload_daemon)(void *ctx, const char *unit);   /* SIGHUP via pidfile   */
    int (*restart_daemon)(void *ctx, const char *unit);  /* systemctl restart    */
    int (*modreload)(void *ctx, const char *unit);       /* stop->modprobe-r/->start */
    int (*set_player)(void *ctx, int enabled);           /* systemctl enable/disable --now pivid-player */
    int (*set_display)(void *ctx, int composite, int tv_standard); /* pivid-display.sh */
    void *ctx;
} ApplyOps;

typedef enum {
    APPLY_RELOAD, APPLY_RESTART, APPLY_MODRELOAD, APPLY_NETCONFIG, APPLY_ENABLED, APPLY_DISABLED, APPLY_BOOTCFG
} ApplyOutcome;
typedef struct { const char *key; ApplyOutcome outcome; } ApplyResult;

/* Diff over config_schema (role-gated), route by tier, do AT MOST ONE radio
 * action. Precedence (highest wins): MODRELOAD > RESTART > LIVE(reload). A
 * NETCONFIG change writes pivid.conf but forces no radio action. The daemon
 * unit is derived from the agent role (so ROLE_BOTH restart/modreload keys need
 * no restart_unit). Per-changed-key outcomes fill results[0..*n-1]. Returns 0 on
 * success, -1 if ANY invoked op failed: write_conf, and on the modreload path
 * both regen_modprobe and modreload, are all attempted and every failure is
 * surfaced (not swallowed). op_modreload itself aborts the unload when the chip
 * refcount never reaches 0 and returns -1.
 * A TIER_CASTER change (the Forward-to-PC toggle) SIGHUPs pivid-caster
 * independently of the single radio action, so it fires even alongside a radio
 * restart/modreload.
 * A TIER_PLAYER change (the Local-HDMI-player toggle) enables/disables
 * pivid-player.service independently of the radio action (RX only). */
int apply_engine_run(AgentRole role,
                     const Settings *current, const Settings *incoming,
                     const ApplyOps *ops,
                     ApplyResult *results, size_t cap, size_t *n);
#endif
