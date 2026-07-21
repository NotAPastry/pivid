#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "role.h"
#include "settings.h"
#include "config_schema.h"
#include "apply_engine.h"
#include "modprobe_gen.h"
#include "sysstat.h"
#include "statusfile.h"
#include "schema_json.h"
#include "link_capacity.h"
#include "recipe_defaults.h"
#include "webui.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Build stamp, defined in main.o (force-rebuilt so it tracks the current tree). */
extern const char *g_pivid_version;

/* ---- real apply ops ----
 * ctx is &AgentRole (a local in the calling handler's stack frame; all ops
 * here run synchronously inside that frame, so the pointer stays valid). */
static int op_write_conf(void *ctx, const Settings *s) {
    (void)ctx; Settings tmp = *s; return settings_save_to_file(&tmp, g_agent_conf_path);
}
static int op_regen_modprobe(void *ctx, const Settings *s) {
    (void)ctx; return modprobe_gen_lr2021(s, AGENT_MODPROBE_CONF);
}
static int op_reload_daemon(void *ctx, const char *unit) {
    (void)ctx;
    /* Pidfile convention is uniform: /run/pivid/<unit>.pid (pivid-rx, pivid-tx,
     * pivid-caster all write their own). Resolve from the unit so a TIER_CASTER
     * reload SIGHUPs pivid-caster, not the role daemon. */
    char path[80];
    if (!unit || snprintf(path, sizeof path, "/run/pivid/%s.pid", unit) >= (int)sizeof path)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int pid = 0; int got = fscanf(f, "%d", &pid); fclose(f);
    if (got != 1 || pid <= 1) return -1;
    return kill((pid_t)pid, SIGHUP);
}
static int op_restart_daemon(void *ctx, const char *unit) {
    (void)ctx;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/systemctl", "systemctl", "restart", unit, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0; (void)waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* Enable+start (on) or disable+stop (off) the local HDMI player unit. `enable
 * --now` makes "on" mean start now AND at next boot; `disable --now` stops it
 * and keeps it down across reboots. Guard: refuse to enable when the player
 * binary isn't installed yet (the unit would just crashloop).
 *
 * Enable uses `--no-block`: the unit's ExecStartPre waits for a connected HDMI
 * display before launching, so a blocking `start` would hang this handler (and
 * the dashboard Apply) on a headless receiver. --no-block enqueues the start
 * and returns immediately; the enable symlink (boot persistence) is written
 * synchronously regardless. Disable's stop is fast, so it stays blocking and
 * keeps accurate success/failure reporting. */
static int op_player_set(void *ctx, int enabled) {
    (void)ctx;
    if (enabled && access("/opt/pivid/pivid-player", X_OK) != 0) {
        fprintf(stderr, "op_player_set: /opt/pivid/pivid-player missing; "
                        "build it (`make player`) and redeploy before enabling\n");
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (enabled)
            execl("/usr/bin/systemctl", "systemctl", "--no-block", "enable",
                  "--now", "pivid-player.service", (char *)NULL);
        else
            execl("/usr/bin/systemctl", "systemctl", "disable", "--now",
                  "pivid-player.service", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0; (void)waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* Set the local display output via pivid-display.sh. composite!=0 -> analog
 * composite at the given standard (0=PAL,1=NTSC); composite==0 -> HDMI (removes
 * the managed block). Edits config.txt; effect is deferred to the next reboot. */
static int op_display_set(void *ctx, int composite, int tv_standard) {
    (void)ctx;
    pid_t pid = fork();
    if (pid == 0) {
        if (composite)
            execl(AGENT_DISPLAY_HELPER, "pivid-display.sh", "set", "composite",
                  tv_standard ? "ntsc" : "pal", (char *)NULL);
        else
            execl(AGENT_DISPLAY_HELPER, "pivid-display.sh", "set", "hdmi", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0; (void)waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* Read /sys/module/lr2021/refcnt; -1 if the module is absent/unreadable. */
static int lr2021_refcnt(void) {
    FILE *f = fopen("/sys/module/lr2021/refcnt", "r");
    if (!f) return -1;
    int rc = -1; if (fscanf(f, "%d", &rc) != 1) rc = -1;
    fclose(f);
    return rc;
}

static int run_sync(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execv(path, argv); _exit(127); }
    if (pid < 0) return -1;
    int st = 0; (void)waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* VideoCore throttle/undervolt bitmask via `vcgencmd get_throttled`. Returns -1
 * when vcgencmd is missing or the output can't be parsed (field then omitted).
 * Fixed command string, no user input. */
static long read_throttle_bits(void) {
    FILE *f = popen("/usr/bin/vcgencmd get_throttled 2>/dev/null", "r");
    if (!f) return -1;
    char buf[64];
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    buf[got] = '\0';
    pclose(f);
    return sysstat_throttle_bits(buf);
}

static int op_modreload(void *ctx, const char *unit) {
    (void)ctx;
    /* 1) stop the daemon so it closes /dev/radio0. */
    { char u[64]; snprintf(u, sizeof u, "%s", unit);
      char *av[] = { "systemctl", "stop", u, NULL };
      run_sync("/usr/bin/systemctl", av); }
    /* 2) wait until the module refcount hits 0 (the daemon child may briefly
     *    outlive the unit's MAINPID shell). Bounded ~3 s. */
    int settled = 0;
    for (int i = 0; i < 30; i++) {
        int rc = lr2021_refcnt();
        if (rc <= 0) { settled = 1; break; }   /* 0 = free; -1 = already unloaded */
        struct timespec ts = { 0, 100000000L }; nanosleep(&ts, NULL);
    }
    if (!settled) {
        fprintf(stderr, "op_modreload: %s still holds lr2021 (refcnt>0); aborting unload\n", unit);
        char u[64]; snprintf(u, sizeof u, "%s", unit);
        char *av[] = { "systemctl", "start", u, NULL };
        run_sync("/usr/bin/systemctl", av);    /* never leave the unit down */
        return -1;
    }
    /* 3) modprobe -r ; check status. */
    { char *av[] = { "modprobe", "-r", "lr2021", NULL };
      if (run_sync("/sbin/modprobe", av) != 0) {
          fprintf(stderr, "op_modreload: modprobe -r lr2021 failed; aborting\n");
          char u[64]; snprintf(u, sizeof u, "%s", unit);
          char *av2[] = { "systemctl", "start", u, NULL };
          run_sync("/usr/bin/systemctl", av2);
          return -1;
      } }
    /* 4) modprobe (loads with the freshly regenerated modprobe.d params). */
    int load_ok = 1;
    { char *av[] = { "modprobe", "lr2021", NULL };
      if (run_sync("/sbin/modprobe", av) != 0) {
          fprintf(stderr, "op_modreload: modprobe lr2021 failed\n");
          load_ok = 0;   /* chip not reloaded -> the apply did NOT succeed */
      } }
    /* 5) start the daemon (always attempt -- never leave the unit down). */
    int rc = 0;
    { char u[64]; snprintf(u, sizeof u, "%s", unit);
      char *av[] = { "systemctl", "start", u, NULL };
      rc = run_sync("/usr/bin/systemctl", av); }
    /* A failed module load dominates: a Type=simple unit's start can return 0
     * even though the chip is running stale params -- don't report success. */
    if (!load_ok) rc = -1;
    return rc;
}

/* ---- response buffer ----
 * civetweb's mg_send_http_ok(conn, type, -1) selects chunked transfer-encoding,
 * after which the body MUST be written via mg_send_chunk (raw mg_printf/mg_write
 * is NOT chunk-framed and yields a malformed, hanging response). To keep the
 * page-building code as plain printf-style appends, we render into a growable
 * buffer and emit it once with a known Content-Length (no chunking). */
typedef struct { char *buf; size_t len, cap; int oom; } Resp;

static void resp_init(Resp *r) {
    r->cap = 8192; r->len = 0; r->oom = 0;
    r->buf = malloc(r->cap);
    if (!r->buf) { r->oom = 1; r->cap = 0; }
}
static void resp_free(Resp *r) { free(r->buf); r->buf = NULL; }

static void resp_reserve(Resp *r, size_t extra) {
    if (r->oom) return;
    if (r->len + extra + 1 <= r->cap) return;
    size_t ncap = r->cap ? r->cap : 8192;
    while (r->len + extra + 1 > ncap) ncap *= 2;
    char *nb = realloc(r->buf, ncap);
    if (!nb) { r->oom = 1; return; }
    r->buf = nb; r->cap = ncap;
}

static void resp_puts(Resp *r, const char *s) {
    if (r->oom) return;
    size_t n = strlen(s);
    resp_reserve(r, n);
    if (r->oom) return;
    memcpy(r->buf + r->len, s, n); r->len += n; r->buf[r->len] = '\0';
}

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
static void resp_printf(Resp *r, const char *fmt, ...) {
    if (r->oom) return;
    va_list ap;
    va_start(ap, fmt);
    char tmp[2048];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { resp_puts(r, tmp); return; }
    /* Rare: output exceeds tmp; grow and re-render. */
    resp_reserve(r, (size_t)n);
    if (r->oom) return;
    va_start(ap, fmt);
    vsnprintf(r->buf + r->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    r->len += (size_t)n; r->buf[r->len] = '\0';
}

/* Emit a buffered body with an explicit Content-Length (no chunking). */
static void resp_send(struct mg_connection *c, Resp *r, const char *mime) {
    if (r->oom || !r->buf) {
        mg_send_http_ok(c, "text/plain", 0);
        resp_free(r);
        return;
    }
    mg_send_http_ok(c, mime, (long long)r->len);
    mg_write(c, r->buf, r->len);
    resp_free(r);
}

/* ---- request body reader ---- */

static int read_body(struct mg_connection *c, char *buf, size_t cap) {
    int n = mg_read(c, buf, cap - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

/* Escape a string for embedding inside a JSON "..." literal: \" \\ and
 * control chars. Writes a NUL-terminated result into out[outcap]. */
static void json_escape(char *out, size_t outcap, const char *in) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < outcap; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch == '\n') { if (o + 2 < outcap) { out[o++]='\\'; out[o++]='n'; } }
        else if (ch == '\r') { if (o + 2 < outcap) { out[o++]='\\'; out[o++]='r'; } }
        else if (ch == '\t') { if (o + 2 < outcap) { out[o++]='\\'; out[o++]='t'; } }
        else if (ch < 0x20)  { /* drop other control chars */ }
        else out[o++] = (char)ch;
    }
    out[o] = '\0';
}

/* Apply a flat JSON object {"K":"v",...} onto s via settings_set_field_str.
 * Minimal: handles string values with \" \\ escapes; ignores nesting.
 * Unknown keys are ignored. On the FIRST known key whose value is rejected,
 * copies that key into badkey[badcap] and returns -1 (s left partially applied
 * but the caller discards it on failure). Returns 0 if all known keys applied. */
static int json_apply(const char *body, Settings *s, char *badkey, size_t badcap) {
    if (badcap) badkey[0] = '\0';
    const char *p = body;
    while ((p = strchr(p, '"')) != NULL) {
        char key[64]; size_t k = 0;
        p++; while (*p && *p != '"' && k + 1 < sizeof key) { key[k++] = *p++; }
        key[k] = '\0';
        if (*p != '"') continue;   /* oversized/truncated key -> resync at next quote */
        p++;
        while (*p == ' ' || *p == ':') p++;
        if (*p != '"') { continue; }       /* only string values */
        p++;
        char val[256]; size_t v = 0;
        while (*p && *p != '"' && v + 1 < sizeof val) {
            if (*p == '\\' && p[1]) { p++; }
            val[v++] = *p++;
        }
        val[v] = '\0';
        if (*p == '"') p++;
        if (settings_set_field_str(s, key, val) == -1) {
            char probe[SETTINGS_FIELD_STR_MAX];
            if (settings_get_field_str(s, key, probe, sizeof probe) == 0) {
                /* known key, bad value -> reject */
                snprintf(badkey, badcap, "%s", key);
                return -1;
            }
            /* unknown key -> ignore, continue */
        }
    }
    return 0;
}

/* ---- JSON GET handlers ---- */

/* GET / -> the embedded dashboard asset. */
static int h_root(struct mg_connection *c, void *cb) {
    (void)cb;
    size_t n = sizeof(WEBUI_HTML) - 1;
    mg_send_http_ok(c, "text/html; charset=utf-8", (long long)n);
    mg_write(c, WEBUI_HTML, n);
    return 200;
}

/* GET /api/schema */
static int h_api_schema(struct mg_connection *c, void *cb) {
    AgentRole role = *(AgentRole *)cb;
    char sbuf[32768];   /* stack-local: civetweb runs a thread pool, no shared static */
    int n = schema_json_build(role, sbuf, sizeof sbuf);
    if (n < 0) { mg_send_http_ok(c, "application/json", 2); mg_write(c, "[]", 2); return 200; }
    mg_send_http_ok(c, "application/json", (long long)n);
    mg_write(c, sbuf, (size_t)n);
    return 200;
}

/* GET /api/config -> {role,config_mode,values:{...}} */
static int h_api_config(struct mg_connection *c, void *cb) {
    AgentRole role = *(AgentRole *)cb;
    Settings s; if (settings_load_from_file(&s, g_agent_conf_path) != 0) memset(&s, 0, sizeof s);
    char vbuf[16384];   /* stack-local: civetweb thread pool, no shared static */
    int vn = config_json_build(&s, role, vbuf, sizeof vbuf);
    if (vn < 0) { vbuf[0] = '{'; vbuf[1] = '}'; vbuf[2] = '\0'; }
    int config_mode = (access(AGENT_CONFIG_MODE_FLAG, F_OK) == 0);
    Resp R; resp_init(&R); Resp *r = &R;
    resp_printf(r, "{\"role\":\"%s\",\"config_mode\":%s,\"values\":%s}",
                role == AGENT_ROLE_RX ? "rx" : "tx",
                config_mode ? "true" : "false", vbuf);
    resp_send(c, r, "application/json");
    return 200;
}

/* GET /api/defaults -> {values:{...}} factory defaults, for the Reset button.
 * Same serialisation as /api/config but from a defaults-initialised Settings. */
static int h_api_defaults(struct mg_connection *c, void *cb) {
    AgentRole role = *(AgentRole *)cb;
    Settings s; settings_defaults(&s);
    char vbuf[16384];
    int vn = config_json_build(&s, role, vbuf, sizeof vbuf);
    if (vn < 0) { vbuf[0] = '{'; vbuf[1] = '}'; vbuf[2] = '\0'; }
    Resp R; resp_init(&R); Resp *r = &R;
    resp_printf(r, "{\"values\":%s}", vbuf);
    resp_send(c, r, "application/json");
    return 200;
}

/* GET /api/recipe-defaults -> {"defaults":{"<recipe>":{"<KEY>":{"v":"<val>","show":true|false},...},...}}
 * The dashboard fetches this once and pre-fills the dependent encoder/FEC fields
 * when the user changes ENCODER_RECIPE. All keys are TX-only; an RX agent
 * never renders the recipe select, so no role gating is needed here. */
static int h_api_recipe_defaults(struct mg_connection *c, void *cb) {
    (void)cb;
    char dbuf[8192];
    int dn = recipe_defaults_json_build(dbuf, sizeof dbuf);
    if (dn < 0) { dbuf[0] = '{'; dbuf[1] = '}'; dbuf[2] = '\0'; }
    Resp R; resp_init(&R); Resp *r = &R;
    resp_printf(r, "{\"defaults\":%s}", dbuf);
    resp_send(c, r, "application/json");
    return 200;
}

/* ---- JSON apply / exit / wifi handlers ---- */

static const char *outcome_word(ApplyOutcome o) {
    switch (o) { case APPLY_RELOAD: return "live reload";
                 case APPLY_RESTART: return "reload";
                 case APPLY_MODRELOAD: return "module reload";
                 case APPLY_ENABLED: return "player enabled";
                 case APPLY_DISABLED: return "player disabled";
                 case APPLY_BOOTCFG: return "reboot required to switch display output";
                 default: return "saved (applies on next config mode)"; }
}

static int h_api_apply(struct mg_connection *c, void *cb) {
    AgentRole role = *(AgentRole *)cb;
    char body[8192]; read_body(c, body, sizeof body);
    Settings cur; settings_load_from_file(&cur, g_agent_conf_path);
    Settings in = cur;
    char badkey[64];
    if (json_apply(body, &in, badkey, sizeof badkey) != 0) {
        char esc[128]; json_escape(esc, sizeof esc, badkey);
        Resp R; resp_init(&R); Resp *r = &R;
        resp_printf(r, "{\"ok\":false,\"action_taken\":\"invalid value for %s\",\"changes\":0}", esc);
        resp_send(c, r, "application/json");
        return 200;
    }
    char verr[128];
    if (settings_validate(&in, verr, sizeof verr) != 0) {
        char esc[256]; json_escape(esc, sizeof esc, verr);
        Resp R; resp_init(&R); Resp *r = &R;
        resp_printf(r, "{\"ok\":false,\"action_taken\":\"%s\",\"changes\":0}", esc);
        resp_send(c, r, "application/json");
        return 200;
    }
    ApplyOps ops = { op_write_conf, op_regen_modprobe, op_reload_daemon,
                     op_restart_daemon, op_modreload, op_player_set,
                     op_display_set, &role };
    ApplyResult res[32]; size_t nr = 0;
    int rc = apply_engine_run(role, &cur, &in, &ops, res, 32, &nr);
    /* Headline = the one radio action if any; else the (netconfig) outcome. */
    const char *action = "no change";
    for (size_t i = 0; i < nr; i++) {
        action = outcome_word(res[i].outcome);
        if (res[i].outcome != APPLY_NETCONFIG) break;
    }
    Resp R; resp_init(&R); Resp *r = &R;
    resp_printf(r, "{\"ok\":%s,\"action_taken\":\"%s\",\"changes\":%zu}",
                rc == 0 ? "true" : "false", action, nr);
    resp_send(c, r, "application/json");
    return 200;
}

static int h_api_exit(struct mg_connection *c, void *cb) {
    AgentRole role = *(AgentRole *)cb;
    char body[8192]; read_body(c, body, sizeof body);
    if (access(AGENT_CONFIG_MODE_FLAG, F_OK) != 0) {
        static const char *NO = "{\"ok\":false,\"reason\":\"mode\"}";
        mg_send_http_ok(c, "application/json", (long long)strlen(NO));
        mg_write(c, NO, strlen(NO));
        return 200;
    }
    Settings cur; settings_load_from_file(&cur, g_agent_conf_path);
    Settings in = cur;
    char badkey[64];
    if (json_apply(body, &in, badkey, sizeof badkey) != 0) {
        char esc[128]; json_escape(esc, sizeof esc, badkey);
        char msg[192]; snprintf(msg, sizeof msg, "{\"ok\":false,\"reason\":\"invalid value for %s\"}", esc);
        mg_send_http_ok(c, "application/json", (long long)strlen(msg));
        mg_write(c, msg, strlen(msg));
        return 200;
    }
    char verr[128];
    if (settings_validate(&in, verr, sizeof verr) != 0) {
        char esc[256]; json_escape(esc, sizeof esc, verr);
        char msg[320]; snprintf(msg, sizeof msg, "{\"ok\":false,\"reason\":\"%s\"}", esc);
        mg_send_http_ok(c, "application/json", (long long)strlen(msg));
        mg_write(c, msg, strlen(msg));
        return 200;
    }
    Settings save = in; settings_save_to_file(&save, g_agent_conf_path);
    modprobe_gen_lr2021(&in, AGENT_MODPROBE_CONF);

    static const char *OK = "{\"ok\":true}";
    mg_send_http_ok(c, "application/json", (long long)strlen(OK));
    mg_write(c, OK, strlen(OK));

    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            const char *daemon = role_daemon_unit(role);
            execl("/bin/sh", "sh", "-c",
                  "sleep 1; systemctl restart \"$1\"; "
                  "systemctl --no-block stop pivid-config-net.service",
                  "sh", daemon, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) { int st; (void)waitpid(pid, &st, 0); }
    return 200;
}

/* GET /api/wifi -> {"ssid":"...","psk_set":bool} parsed from the current
 * wpa_supplicant.conf via the wifi-client helper's `get` subcommand, so the
 * dashboard's SSID field pre-populates with the network the unit currently
 * joins. The helper path is a fixed compile-time constant (no user input), so
 * popen() here carries no injection surface. */
static int wifi_get_json(Resp *r) {
    char ssid[256] = {0}; int psk_set = 0;
    FILE *p = popen(AGENT_WIFI_HELPER " get 2>/dev/null", "r");
    if (p) {
        char line[320];
        while (fgets(line, sizeof line, p)) {
            if (strncmp(line, "SSID=", 5) == 0) {
                char raw[160]; size_t i = 0; const char *s = line + 5;
                while (s[i] && s[i] != '\n' && i < sizeof raw - 1) { raw[i] = s[i]; i++; }
                raw[i] = '\0';
                schema_json_escape(raw, ssid, sizeof ssid);   /* JSON-correct */
            } else if (strncmp(line, "PSK_SET=", 8) == 0) {
                psk_set = (line[8] == '1');
            }
        }
        pclose(p);
    }
    resp_printf(r, "{\"ssid\":\"%s\",\"psk_set\":%s}", ssid, psk_set ? "true" : "false");
    return 0;
}

static int h_api_wifi(struct mg_connection *c, void *cb) {
    (void)cb;
    const struct mg_request_info *ri = mg_get_request_info(c);
    if (ri && ri->request_method && strcmp(ri->request_method, "GET") == 0) {
        Resp R; resp_init(&R); Resp *r = &R;
        wifi_get_json(r);
        resp_send(c, r, "application/json");
        return 200;
    }
    char body[2048]; read_body(c, body, sizeof body);
    char ssid[128] = {0}, psk[128] = {0};
    {
        const char *q;
        if ((q = strstr(body, "\"ssid\""))) { q = strchr(q + 6, '"'); if (q) { q++; size_t i=0; while(*q&&*q!='"'&&i<sizeof ssid-1){if(*q=='\\'&&q[1])q++;ssid[i++]=*q++;} ssid[i]=0; } }
        if ((q = strstr(body, "\"psk\"")))  { q = strchr(q + 5, '"'); if (q) { q++; size_t i=0; while(*q&&*q!='"'&&i<sizeof psk-1){if(*q=='\\'&&q[1])q++;psk[i++]=*q++;} psk[i]=0; } }
    }
    if (ssid[0] == '\0') {
        static const char *NO = "{\"ok\":false,\"reason\":\"ssid\"}";
        mg_send_http_ok(c, "application/json", (long long)strlen(NO));
        mg_write(c, NO, strlen(NO));
        return 200;
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (psk[0])
            execl(AGENT_WIFI_HELPER, "pivid-wifi-client.sh", "set", ssid, psk, (char *)NULL);
        else
            execl(AGENT_WIFI_HELPER, "pivid-wifi-client.sh", "set", ssid, (char *)NULL);
        _exit(127);
    }
    int st = 0; if (pid > 0) (void)waitpid(pid, &st, 0);
    static const char *OK = "{\"ok\":true}";
    mg_send_http_ok(c, "application/json", (long long)strlen(OK));
    mg_write(c, OK, strlen(OK));
    return 200;
}

/* POST /api/reboot -> reboot the unit. Detached double-fork so the JSON response
 * flushes before shutdown. DietPi masks logind, so plain `reboot` fails
 * ("File exists") -- use `systemctl --force reboot`. */
static int h_api_reboot(struct mg_connection *c, void *cb) {
    (void)cb;
    static const char *OK = "{\"ok\":true}";
    mg_send_http_ok(c, "application/json", (long long)strlen(OK));
    mg_write(c, OK, strlen(OK));

    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c",
                  "sleep 1; systemctl --force reboot", "sh", (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) { int st; (void)waitpid(pid, &st, 0); }
    return 200;
}

static int h_api_stats(struct mg_connection *c, void *cb) {
    AgentRole role = *(AgentRole *)cb;
    char raw[8192]; StatusMap m[STATUSFILE_MAX_KV]; size_t n = 0;
    const char *keys[] = { "rssi_dbm_neg","k_crc","k_crc_drops","k_sync","k_len",
                           "k_tmout","k_rx_silent_ms","k_ring_full","dropped_full",
                           "tx_kbps","rx_kbps",
                           /* MSP DisplayPort relay (TX role only; absent -> null) */
                           "msp_fc_link_state","msp_polls_ok","msp_parse_resets",
                           "msp_crc_failures","msp_kf_frags_tx",
                           /* OSD / telemetry-RX (RX role only; absent -> null) */
                           "osd_enabled","osd_state","osd_frames","osd_idrs",
                           "telem_age_ms","vid_rssi_dbm_neg","vid_lq_pct",
                           "rx_video_pkts",
                           /* OSD-grid receive-health (RX role only; absent -> null) */
                           "msp_rx_ok","msp_rx_err",
                           "osd_frags_rx","osd_frames_done",
                           "osd_frames_dropped","osd_frags_stale" };
    if (statusfile_read(role_status_file(role), raw, sizeof raw) == 0)
        n = statusfile_parse(raw, m, STATUSFILE_MAX_KV);
    Resp R; resp_init(&R); Resp *r = &R;
    resp_puts(r, "{");
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        const char *v = statusfile_get(m, n, keys[i]);
        resp_printf(r, "%s\"%s\":%s", i ? "," : "", keys[i], v ? v : "null");
    }
    const char *tk = statusfile_get(m, n, "tx_kbps");
    const char *rk = statusfile_get(m, n, "rx_kbps");
    resp_printf(r, ",\"kbps\":%s", tk ? tk : (rk ? rk : "null"));

    /* Theoretical ceilings derived from the current config (independent of the
     * live stat): air = over-the-air payload ceiling, video = slice left after
     * software FEC. Best-effort -- omit (null) if settings can't be read. */
    Settings s;
    if (settings_load_from_file(&s, g_agent_conf_path) == 0) {
        uint64_t air = 0, video = 0;
        /* Conservative video budget: charge the anchor m to all video (worst
         * case = all anchors). With ref-P at m=0 the true overhead is lower, so
         * this under-promises the budget -- the safe direction for a planning
         * ceiling. link_capacity()'s signature is unchanged (single m). */
        link_capacity(s.flrc_cr, s.flrc_pld_len,
                      s.fec_m_anchor, s.fec_k, &air, &video);
        resp_printf(r, ",\"max_kbps\":%llu", (unsigned long long)air);
        /* The usable-video budget is a TX-encoder planning number ("keep the
         * encoder bitrate under this"); it isn't actionable on RX, so omit it
         * there. Air max (the link ceiling) is symmetric and shown on both. */
        if (role == AGENT_ROLE_TX)
            resp_printf(r, ",\"video_kbps\":%llu", (unsigned long long)video);
        else
            resp_puts(r, ",\"video_kbps\":null");
    } else {
        resp_puts(r, ",\"max_kbps\":null,\"video_kbps\":null");
    }

    /* Stream uptime: how long the pivid daemon (and thus this stream) has been
     * running, so the counters above have a denominator. Derived from the
     * daemon's process start without touching the daemon itself. */
    long uptime_s = -1;
    char pidbuf[32] = {0};
    const char *pidfile = (role == AGENT_ROLE_RX) ? AGENT_RX_PIDFILE
                                                  : AGENT_TX_PIDFILE;
    if (statusfile_read(pidfile, pidbuf, sizeof pidbuf) == 0) {
        long pid = strtol(pidbuf, NULL, 10);
        if (pid > 0) {
            char statpath[64], upbuf[64] = {0}, statbuf[2048] = {0};
            snprintf(statpath, sizeof statpath, "/proc/%ld/stat", pid);
            if (statusfile_read("/proc/uptime", upbuf, sizeof upbuf) == 0 &&
                statusfile_read(statpath, statbuf, sizeof statbuf) == 0)
                uptime_s = sysstat_stream_uptime_secs(upbuf, statbuf,
                                                      sysconf(_SC_CLK_TCK));
        }
    }
    if (uptime_s >= 0) resp_printf(r, ",\"uptime_s\":%ld", uptime_s);
    else               resp_puts(r, ",\"uptime_s\":null");

    resp_printf(r, ",\"have\":%s}", n ? "true" : "false");
    resp_send(c, r, "application/json");
    return 200;
}

static int h_api_sysstatus(struct mg_connection *c, void *cb) {
    (void)cb;
    char stat[8192] = {0}, mem[4096] = {0}, th[64] = {0};

    int cpu = 0;
    CpuTimes a, b;
    if (statusfile_read("/proc/stat", stat, sizeof stat) == 0 &&
        sysstat_parse_cpu(stat, &a) == 0) {
        struct timespec ts = { 0, 200000000L };
        nanosleep(&ts, NULL);
        stat[0] = '\0';
        if (statusfile_read("/proc/stat", stat, sizeof stat) == 0 &&
            sysstat_parse_cpu(stat, &b) == 0)
            cpu = sysstat_cpu_busy_pct(&a, &b);
    }

    int used = -1;
    if (statusfile_read("/proc/meminfo", mem, sizeof mem) == 0)
        used = sysstat_mem_used_pct(mem);

    int tc = -1;
    if (statusfile_read("/sys/class/thermal/thermal_zone0/temp", th, sizeof th) == 0)
        tc = sysstat_thermal_c(th);

    /* Kernel string: "<sysname> <release>"; controlled, but JSON-escape it. */
    char kernel[160] = "unknown";
    struct utsname u;
    if (uname(&u) == 0) {
        char raw[160];
        snprintf(raw, sizeof raw, "%s %s", u.sysname, u.release);
        schema_json_escape(raw, kernel, sizeof kernel);   /* JSON-correct escaping */
    }

    /* Authoritative config-mode for the dashboard's Apply&Exit gate: the AP is
     * only really up while pivid-config-net.service is active. Using the service
     * state (not just the /run flag, which a hard exit could orphan) guarantees
     * the button never shows when AP mode is off. */
    char *avm[] = { "systemctl", "is-active", "--quiet", AGENT_NET_UNIT, NULL };
    int config_mode = (run_sync("/usr/bin/systemctl", avm) == 0);

    Resp R; resp_init(&R); Resp *r = &R;
    /* g_pivid_version lives in main.o, which the Makefile force-rebuilds so the
     * stamp always tracks the current tree (a -D change alone won't rebuild a TU
     * whose source mtime is unchanged). Keeping it out of this 80 KB-literal TU
     * avoids recompiling http.o on every build. config_mode is polled here too so
     * the dashboard can live-hide the Apply&Exit button the moment AP mode ends. */
    long thr = read_throttle_bits();
    resp_printf(r, "{\"kernel\":\"%s\",\"cpu_pct\":%d,\"mem_pct\":%d,\"temp_c\":%d,"
                   "\"version\":\"%s\",\"config_mode\":%s",
                kernel, cpu, used, tc, g_pivid_version, config_mode ? "true" : "false");
    if (thr >= 0) resp_printf(r, ",\"throttled\":%ld", thr);
    else          resp_puts(r, ",\"throttled\":null");
    resp_puts(r, "}");
    resp_send(c, r, "application/json");
    return 200;
}

static int h_captive(struct mg_connection *c, void *cb) {
    (void)cb;
    mg_printf(c, "HTTP/1.1 302 Found\r\nLocation: http://10.0.0.1/\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n");
    return 302;
}

void http_register(struct mg_context *ctx, AgentRole role) {
    static AgentRole r; r = role;
    /* Order matters: civetweb longest-prefix match, specific paths first. */
    mg_set_request_handler(ctx, "/api/stats",     h_api_stats,     &r);
    mg_set_request_handler(ctx, "/api/sysstatus", h_api_sysstatus, &r);
    mg_set_request_handler(ctx, "/api/schema",    h_api_schema,    &r);
    mg_set_request_handler(ctx, "/api/config",    h_api_config,    &r);
    mg_set_request_handler(ctx, "/api/defaults",         h_api_defaults,         &r);
    mg_set_request_handler(ctx, "/api/recipe-defaults",  h_api_recipe_defaults,  &r);
    mg_set_request_handler(ctx, "/api/apply",     h_api_apply,     &r);
    mg_set_request_handler(ctx, "/api/exit",      h_api_exit,      &r);
    mg_set_request_handler(ctx, "/api/wifi",      h_api_wifi,      &r);
    mg_set_request_handler(ctx, "/api/reboot",    h_api_reboot,    &r);
    mg_set_request_handler(ctx, "/",              h_root,          &r);
    /* Captive-portal probes -> redirect to the portal. */
    mg_set_request_handler(ctx, "/generate_204",       h_captive, &r);
    mg_set_request_handler(ctx, "/gen_204",            h_captive, &r);
    mg_set_request_handler(ctx, "/hotspot-detect.html",h_captive, &r);
    mg_set_request_handler(ctx, "/ncsi.txt",           h_captive, &r);
    mg_set_request_handler(ctx, "/connecttest.txt",    h_captive, &r);
    mg_set_request_handler(ctx, "/canonical.html",     h_captive, &r);
}
