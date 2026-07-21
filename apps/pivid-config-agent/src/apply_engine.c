#define _POSIX_C_SOURCE 200809L
#include "apply_engine.h"
#include "config_schema.h"
#include <string.h>

static int key_for_role(ConfigRole kr, AgentRole ar) {
    if (kr == ROLE_BOTH) return 1;
    if (kr == ROLE_TX)   return ar == AGENT_ROLE_TX;
    return ar == AGENT_ROLE_RX;   /* ROLE_RX */
}

int apply_engine_run(AgentRole role,
                     const Settings *current, const Settings *incoming,
                     const ApplyOps *ops,
                     ApplyResult *results, size_t cap, size_t *n) {
    int need_reload = 0, need_restart = 0, need_modreload = 0, need_netconfig = 0;
    int need_caster = 0, need_player = 0, need_bootcfg = 0;
    size_t nr = 0;
    const char *daemon = (role == AGENT_ROLE_RX) ? "pivid-rx" : "pivid-tx";

    for (size_t i = 0; i < config_schema_len; i++) {
        const ConfigKeyMeta *m = &config_schema[i];
        if (!key_for_role(m->role, role)) continue;

        char a[SETTINGS_FIELD_STR_MAX], b[SETTINGS_FIELD_STR_MAX];
        if (settings_get_field_str(current,  m->key, a, sizeof a) != 0) continue;
        if (settings_get_field_str(incoming, m->key, b, sizeof b) != 0) continue;
        if (strcmp(a, b) == 0) continue;          /* unchanged */

        ApplyOutcome oc;
        switch (m->tier) {
            case TIER_LIVE:      need_reload = 1;    oc = APPLY_RELOAD;    break;
            case TIER_RESTART:   need_restart = 1;   oc = APPLY_RESTART;   break;
            case TIER_MODRELOAD: need_modreload = 1; oc = APPLY_MODRELOAD; break;
            case TIER_CASTER:    need_caster = 1;    oc = APPLY_RELOAD;    break;
            case TIER_PLAYER:
                need_player = 1;
                oc = incoming->player_local ? APPLY_ENABLED : APPLY_DISABLED;
                break;
            case TIER_BOOTCFG:
                need_bootcfg = 1;
                oc = APPLY_BOOTCFG;
                break;
            default:             need_netconfig = 1; oc = APPLY_NETCONFIG; break; /* NETCONFIG */
        }
        if (nr < cap) { results[nr].key = m->key; results[nr].outcome = oc; }
        nr++;
    }
    *n = (nr < cap) ? nr : cap;

    /* Nothing changed at all -> no-op success. */
    if (!need_reload && !need_restart && !need_modreload && !need_netconfig &&
        !need_caster && !need_player && !need_bootcfg)
        return 0;

    /* Persist first; if the write fails, abort the apply entirely -- do not
     * signal any daemon (the on-disk + live state both stay on the old config). */
    if (ops->write_conf(ops->ctx, incoming) != 0)
        return -1;

    int rc = 0;
    /* One radio action, highest precedence wins (modreload > restart > reload). */
    if (need_modreload) {
        /* Regenerate the boot defaults, then reload the module. A failed unload
         * (refcnt never 0 / modprobe -r != 0) is reported by op_modreload as -1;
         * it leaves the daemon running, and we do NOT pretend success. */
        if (ops->regen_modprobe(ops->ctx, incoming) != 0) rc = -1;
        if (ops->modreload(ops->ctx, daemon) != 0) rc = -1;
    } else if (need_restart) {
        if (ops->restart_daemon(ops->ctx, daemon) != 0) rc = -1;
    } else if (need_reload) {
        if (ops->reload_daemon(ops->ctx, daemon) != 0) rc = -1;
    }

    /* Caster reload is INDEPENDENT of the radio action: pivid-caster never opens
     * /dev/radio0, so the "Forward to PC" toggle SIGHUPs it regardless of any
     * concurrent radio restart/modreload. */
    if (need_caster) {
        if (ops->reload_daemon(ops->ctx, "pivid-caster") != 0) rc = -1;
    }
    /* Player toggle is INDEPENDENT of the radio action, like the caster: the
     * local player never opens /dev/radio0. enable/disable --now pivid-player. */
    if (need_player) {
        if (ops->set_player(ops->ctx, incoming->player_local) != 0) rc = -1;
    }
    /* Composite/HDMI display switch is INDEPENDENT of the radio action, like the
     * caster/player: the display helper never opens /dev/radio0. It edits
     * config.txt; the change takes effect on the next reboot. */
    if (need_bootcfg) {
        if (ops->set_display(ops->ctx, incoming->player_composite,
                             incoming->player_tv_standard) != 0) rc = -1;
    }
    /* NETCONFIG-only changes need just the conf write above. */
    return rc;
}
