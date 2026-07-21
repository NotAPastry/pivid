#ifndef AGENT_MODPROBE_GEN_H
#define AGENT_MODPROBE_GEN_H
#include "settings.h"
/* Atomically (re)write `path` with `options lr2021 flrc_*=...` for all 7 flrc
 * params from `s`. Emits the conf values verbatim on both roles: an RX-role
 * "-18 dBm PA pin" was tried 2026-07-06 and REVERTED the same day (suspected
 * RSSI interaction, unproven -- A/B not run; keep roles symmetric unless one
 * is). Returns 0 on success, -1 on error (errno preserved). */
int modprobe_gen_lr2021(const Settings *s, const char *path);
#endif
