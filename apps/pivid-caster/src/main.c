/* apps/pivid-caster/src/main.c -- pivid-caster daemon entry point.
 *
 * Single-threaded poll() forwarder (caster.c) plus: settings load, signal
 * handling (SIGINT/SIGTERM shutdown, SIGHUP live-reload of the forwarding
 * toggle, SIGPIPE ignored; LATPROBE builds also handle SIGUSR1 -> probe
 * histogram dump), a pidfile so the config agent can SIGHUP us
 * directly, a /run/pivid/caster.status heartbeat, and mDNS orchestration that
 * follows the toggle. mDNS is best-effort and never gates loopback forwarding.
 */
#define _POSIX_C_SOURCE 200809L
#include "caster.h"
#include "control.h"
#include "loopback.h"
#include "mdns.h"

#include "settings.h"
#include "status_file.h"
#include "cli_args.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef BUILD_VERSION
#define BUILD_VERSION "unknown"
#endif

#define HEARTBEAT_MS  5000

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_reload   = 0;

static const char *g_config_path = SETTINGS_FILE;  /* set from --config in main */

static void on_signal(int sig) { (void)sig; g_shutdown = 1; }
static void on_sighup(int sig) { (void)sig; g_reload   = 1; }

#ifdef LATPROBE
static volatile sig_atomic_t g_dump_req;
static void on_sigusr1(int sig) { (void)sig; g_dump_req = 1; }
#endif

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void write_status(const CasterCtx *c) {
    StatusKV kv[] = {
        { "fwd_on",        (unsigned long long)(c->forward_enabled ? 1 : 0) },
        { "subscribers",   (unsigned long long)subs_count(&c->subs) },
        { "video_in",      c->video_in },
        { "telem_in",      c->telem_in },
        { "video_fwd",     c->video_fwd },
        { "telem_fwd",     c->telem_fwd },
        { "bytes_fwd",     c->bytes_fwd },
        { "subs_rejected", c->subs_rejected },
        { "ctrl_dropped",  c->ctrl_dropped },
        { "oversize_drops", c->oversize_drops },
        { "chunk_send_fail",c->chunk_send_fail },
        { "telem_send_fail",c->telem_send_fail },
    };
    size_t n = sizeof(kv) / sizeof(kv[0]);
    if (status_write("/run/pivid/caster.status", kv, n) < 0) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "pivid-caster: status_write failed (%s); "
                            "continuing without snapshot file\n", strerror(errno));
        }
    }
}

/* Apply the forwarding toggle from pivid.conf to both the gate and mDNS.
 * Turning OFF gates new (non-loopback) SUBSCRIBEs and withdraws mDNS; any remote
 * already in the table keeps receiving until its ~5 s TTL expires (it can no
 * longer renew), then ages out. We deliberately do NOT flush remotes on OFF --
 * the bounded, self-healing TTL drain is simpler and good enough. */
static void apply_forward(CasterCtx *c, CasterMdns *mdns, const Settings *s) {
    caster_set_forward(c, s->forward_to_pc != 0);
    if (s->forward_to_pc)
        mdns_publish(mdns, "PiVid", (uint16_t)CASTER_CTRL_PORT, s->width, s->height, s->fps);
    else
        mdns_withdraw(mdns);
}

int main(int argc, char **argv) {
    CliArgs args;
    if (cli_parse(argc, argv, SETTINGS_FILE, &args) != 0) {
        cli_print_usage(stderr, "pivid-caster");
        return 2;
    }
    if (args.show_help)    { cli_print_usage(stdout, "pivid-caster"); return 0; }
    if (args.show_version) { printf("pivid-caster %s\n", BUILD_VERSION); return 0; }
    if (args.check) {
        char err[128];
        if (settings_check_file(args.config_path, err, sizeof err) != 0) {
            fprintf(stderr, "pivid-caster: config invalid: %s\n", err);
            return 1;
        }
        printf("pivid-caster: config OK\n");
        return 0;
    }
    g_config_path = args.config_path;   /* SIGHUP reload honors --config */

    Settings s;
    char err[128];
    if (settings_load_checked(&s, args.config_path, err, sizeof err) != 0 ||
        settings_validate(&s, err, sizeof err) != 0) {
        fprintf(stderr, "pivid-caster: config invalid: %s\n", err);
        return 1;
    }

    /* tmpfs snapshot dir for the pidfile + status heartbeat. mkdir is
     * idempotent; EEXIST is fine. The caster must not depend on pivid-rx/tx
     * having created /run/pivid first (no service ordering guarantees that). */
    if (mkdir("/run/pivid", 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "pivid-caster: mkdir /run/pivid failed (%s); "
                        "pidfile/status may be unavailable\n", strerror(errno));

    CasterCtx caster;
    if (caster_open(&caster, s.forward_to_pc != 0) < 0) {
        fprintf(stderr, "pivid-caster: failed to open sockets\n");
        return 1;
    }

    struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &(struct sigaction){ .sa_handler = on_sighup, .sa_flags = SA_RESTART }, NULL);
#ifdef LATPROBE
    sigaction(SIGUSR1, &(struct sigaction){ .sa_handler = on_sigusr1, .sa_flags = SA_RESTART }, NULL);
#endif
    struct sigaction sa_pipe = { .sa_handler = SIG_IGN, .sa_flags = 0 };
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);

    /* Pidfile so the config agent can SIGHUP us directly. /run is tmpfs. */
    {
        FILE *pf = fopen("/run/pivid/pivid-caster.pid", "w");
        if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    }

    CasterMdns *mdns = mdns_create();   /* NULL is fine (best-effort) */
    apply_forward(&caster, mdns, &s);

    fprintf(stderr, "pivid-caster: started (%s), forward_to_pc=%d, "
                    "in 127.0.0.1:%d/%d ctrl :%d\n",
            BUILD_VERSION, s.forward_to_pc,
            LOOPBACK_VIDEO_PORT, LOOPBACK_TELEM_PORT, CASTER_CTRL_PORT);

    int64_t last_beat = mono_ms();
    write_status(&caster);

    while (!g_shutdown) {
        int64_t now = mono_ms();
        if (caster_step(&caster, 1000, now) < 0 && errno != EINTR) {
            perror("pivid-caster poll");
            break;
        }
#ifdef LATPROBE
        if (g_dump_req) { g_dump_req = 0; caster_probe_dump(STDERR_FILENO); }
#endif
        if (g_reload) {
            g_reload = 0;
            settings_load_from_file(&s, g_config_path);
            apply_forward(&caster, mdns, &s);
            fprintf(stderr, "pivid-caster: SIGHUP reload, forward_to_pc=%d\n", s.forward_to_pc);
        }
        now = mono_ms();
        if (now - last_beat >= HEARTBEAT_MS) {
            last_beat = now;
            write_status(&caster);
        }
    }

    fprintf(stderr, "pivid-caster: shutting down\n");
    mdns_withdraw(mdns);
    mdns_destroy(mdns);
    caster_close(&caster);
    unlink("/run/pivid/pivid-caster.pid");
    return 0;
}
