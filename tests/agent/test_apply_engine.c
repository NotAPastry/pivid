#include "../apps/pivid-config-agent/src/apply_engine.h"
#include "../common/config_schema.h"
#include "../common/settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int n_write, n_regen, n_reload, n_restart, n_modreload;
static int n_player;          /* set_player call count            */
static int player_arg;        /* last `enabled` arg seen          */
static int n_display;         /* set_display call count */
static int disp_composite, disp_std;  /* last args seen  */
static char last_unit[64];
static int  modreload_rc;   /* return value the mock op_modreload yields */

static void reset(void) {
    n_write = n_regen = n_reload = n_restart = n_modreload = n_player = 0;
    player_arg = -1;
    n_display = 0; disp_composite = disp_std = -1;
    last_unit[0] = '\0'; modreload_rc = 0;
}
static int m_write(void *c, const Settings *s)  { (void)c;(void)s; n_write++;   return 0; }
static int m_regen(void *c, const Settings *s)  { (void)c;(void)s; n_regen++;   return 0; }
static int m_reload(void *c, const char *u)     { (void)c; n_reload++;  snprintf(last_unit,sizeof last_unit,"%s",u); return 0; }
static int m_restart(void *c, const char *u)    { (void)c; n_restart++; snprintf(last_unit,sizeof last_unit,"%s",u); return 0; }
static int m_modreload(void *c, const char *u)  { (void)c; n_modreload++; snprintf(last_unit,sizeof last_unit,"%s",u); return modreload_rc; }
static int m_player(void *c, int en) { (void)c; n_player++; player_arg = en; return 0; }
static int m_display(void *c, int comp, int std) { (void)c; n_display++; disp_composite = comp; disp_std = std; return 0; }

static ApplyOps mkops(void) {
    ApplyOps o = { m_write, m_regen, m_reload, m_restart, m_modreload, m_player, m_display, NULL };
    return o;
}

static void test_fec_is_live(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; in.fec_m_anchor = cur.fec_m_anchor + 1;
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_TX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 1 && r[0].outcome == APPLY_RELOAD);
    assert(n_write == 1 && n_reload == 1 && n_restart == 0 && n_modreload == 0);
    printf("test_fec_is_live: OK\n");
}

static int m_write_fail(void *c, const Settings *s) { (void)c;(void)s; n_write++; return -1; }

static void test_write_failure_aborts_apply(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; in.fec_m_anchor = cur.fec_m_anchor + 1;   /* a live change */
    ApplyOps o = mkops(); o.write_conf = m_write_fail;
    ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_TX, &cur, &in, &o, r, 32, &n) == -1);
    assert(n_write == 1);
    assert(n_reload == 0 && n_restart == 0 && n_modreload == 0);  /* no signal on write fail */
    printf("test_write_failure_aborts_apply: OK\n");
}

static void test_write_failure_aborts_modreload(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; in.flrc_cr = (cur.flrc_cr == 1) ? 2 : 1;   /* modreload-tier key */
    ApplyOps o = mkops(); o.write_conf = m_write_fail;
    ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_TX, &cur, &in, &o, r, 32, &n) == -1);
    assert(n_write == 1);
    assert(n_regen == 0);   /* modprobe.d regen must NOT run on a failed write */
    assert(n_reload == 0 && n_restart == 0 && n_modreload == 0);
    printf("test_write_failure_aborts_modreload: OK\n");
}

static void test_flrc_is_modreload(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; in.flrc_cr = (cur.flrc_cr == 1) ? 2 : 1;
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_TX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 1 && r[0].outcome == APPLY_MODRELOAD);
    assert(n_write == 1 && n_regen == 1 && n_modreload == 1);
    assert(n_restart == 0 && n_reload == 0);
    assert(strcmp(last_unit, "pivid-tx") == 0);
    printf("test_flrc_is_modreload: OK\n");
}

static void test_modreload_subsumes_restart(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur;
    in.flrc_cr = (cur.flrc_cr == 1) ? 2 : 1;   /* modreload key */
    in.bitrate = cur.bitrate + 1000;           /* restart key   */
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_TX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n_modreload == 1 && n_restart == 0 && n_reload == 0);  /* one action */
    printf("test_modreload_subsumes_restart: OK\n");
}

static void test_pairing_is_restart_both_roles(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; strcpy(in.binding_phrase, "different-phrase-here");
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_RX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 1 && r[0].outcome == APPLY_RESTART);
    assert(n_restart == 1 && strcmp(last_unit, "pivid-rx") == 0);  /* role-derived */
    printf("test_pairing_is_restart_both_roles: OK\n");
}

static void test_ap_password_is_netconfig(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; strcpy(in.ap_password, "newsecret1");
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_TX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 1 && r[0].outcome == APPLY_NETCONFIG);
    assert(n_write == 1);                       /* conf written ... */
    assert(n_modreload == 0 && n_restart == 0 && n_reload == 0);  /* ... no radio action */
    printf("test_ap_password_is_netconfig: OK\n");
}

static void test_failed_unload_aborts(void) {
    reset();
    modreload_rc = -1;   /* mock: refcnt never reached 0 / modprobe -r failed */
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; in.rf_tx_power = cur.rf_tx_power - 1;  /* modreload key */
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    int rc = apply_engine_run(AGENT_ROLE_TX, &cur, &in, &o, r, 32, &n);
    assert(rc == -1);                 /* surfaced as failure */
    assert(n_modreload == 1);         /* attempted ... */
    printf("test_failed_unload_aborts: OK\n");
}

static void test_forward_to_pc_reloads_caster(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; in.forward_to_pc = !cur.forward_to_pc;   /* TIER_CASTER key */
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_RX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 1 && r[0].outcome == APPLY_RELOAD);
    assert(n_write == 1);                                   /* conf persisted */
    assert(n_reload == 1 && strcmp(last_unit, "pivid-caster") == 0);
    assert(n_restart == 0 && n_modreload == 0);             /* no radio action */
    printf("test_forward_to_pc_reloads_caster: OK\n");
}

static void test_caster_reload_runs_alongside_restart(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur;
    strcpy(in.binding_phrase, "different-phrase-here"); /* TIER_RESTART, pivid-rx */
    in.forward_to_pc = !cur.forward_to_pc;              /* TIER_CASTER, pivid-caster */
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_RX, &cur, &in, &o, r, 32, &n) == 0);
    /* BOTH actions fire -- the caster reload is NOT subsumed by the radio restart. */
    assert(n_restart == 1);                                 /* pivid-rx restart */
    assert(n_reload == 1 && strcmp(last_unit, "pivid-caster") == 0); /* caster SIGHUP */
    assert(n == 2);                                         /* two changed keys reported */
    printf("test_caster_reload_runs_alongside_restart: OK\n");
}

static void test_osd_cell_is_live_reload(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; in.osd_vid_rssi_row = cur.osd_vid_rssi_row + 1; /* TIER_LIVE, ROLE_RX */
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_RX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 1 && r[0].outcome == APPLY_RELOAD);
    assert(n_write == 1);                                   /* conf persisted */
    assert(n_reload == 1 && strcmp(last_unit, "pivid-rx") == 0); /* SIGHUP the RX */
    assert(n_restart == 0 && n_modreload == 0);             /* no restart */
    printf("test_osd_cell_is_live_reload: OK\n");
}

static void test_player_enable_is_independent(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");   /* player_local=0 */
    Settings in = cur; in.player_local = 1;                          /* turn on */
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_RX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 1 && r[0].outcome == APPLY_ENABLED);
    assert(n_write == 1 && n_player == 1 && player_arg == 1);
    assert(n_restart == 0 && n_reload == 0 && n_modreload == 0);    /* no radio action */
    printf("test_player_enable_is_independent: OK\n");
}

static void test_player_disable_outcome(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    cur.player_local = 1;                 /* currently on */
    Settings in = cur; in.player_local = 0;   /* turn off */
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_RX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 1 && r[0].outcome == APPLY_DISABLED);
    assert(n_player == 1 && player_arg == 0);
    printf("test_player_disable_outcome: OK\n");
}

static void test_player_is_tx_role_gated(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings in = cur; in.player_local = 1;
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    /* On TX the RX-only key is ignored: no change, no op. */
    assert(apply_engine_run(AGENT_ROLE_TX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 0 && n_player == 0 && n_write == 0);
    printf("test_player_is_tx_role_gated: OK\n");
}

static void test_composite_is_bootcfg(void) {
    reset();
    Settings cur; settings_load_from_file(&cur, "/nonexistent");  /* defaults */
    Settings in = cur;
    in.player_composite = 1;
    in.player_tv_standard = 1;   /* NTSC */
    ApplyOps o = mkops(); ApplyResult r[32]; size_t n = 0;
    assert(apply_engine_run(AGENT_ROLE_RX, &cur, &in, &o, r, 32, &n) == 0);
    assert(n == 2);  /* both keys changed */
    assert(r[0].outcome == APPLY_BOOTCFG);
    assert(n_write == 1 && n_display == 1);
    assert(disp_composite == 1 && disp_std == 1);
    /* independent of radio: no restart/modreload/reload fired */
    assert(n_restart == 0 && n_modreload == 0 && n_reload == 0);
    printf("test_composite_is_bootcfg: OK\n");
}

int main(void) {
    test_fec_is_live();
    test_osd_cell_is_live_reload();
    test_flrc_is_modreload();
    test_modreload_subsumes_restart();
    test_pairing_is_restart_both_roles();
    test_ap_password_is_netconfig();
    test_forward_to_pc_reloads_caster();
    test_caster_reload_runs_alongside_restart();
    test_failed_unload_aborts();
    test_write_failure_aborts_apply();
    test_write_failure_aborts_modreload();
    test_player_enable_is_independent();
    test_player_disable_outcome();
    test_player_is_tx_role_gated();
    test_composite_is_bootcfg();
    printf("All apply_engine tests passed!\n");
    return 0;
}
