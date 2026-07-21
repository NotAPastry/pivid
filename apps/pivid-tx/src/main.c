/* pivid-tx daemon entry point.
 *
 * Wires TX_Dev + Encoder + GPIO_Thread together. main() loads settings,
 * opens /dev/radio0, creates the eventfd, and loops on encoder_run().
 * On encoder return == 2 (wake_fd signaled) main has been asked either to
 * shut down (SIGINT/SIGTERM set g_shutdown) or to apply a live reload
 * (SIGHUP from the config agent sets g_reload -> reload_apply re-reads
 * pivid.conf and re-applies the live subset mid-stream). The encoder/radio
 * loop keeps streaming throughout. The config-mode button moved to the
 * always-on pivid-config-agent.
 *
 * The eventfd is the single wake path: signal handlers (and the GPIO
 * thread) write to it. eventfd_write is async-signal-safe per POSIX. */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "tx.h"
#include "tx_sample.h"
#include "aqm.h"
#include "encoder.h"
#include "camera_spawn.h"
#include "gpio_thread.h"
#include "msp_poll.h"
#include "status_file.h"
#include "kbps.h"

#include "peripherals.h"
#include "radio_ioctl.h"
#include "settings.h"
#include "config_schema.h"
#include "cli_args.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static _Atomic int g_shutdown = 0;
static _Atomic int g_reload   = 0;
static int         g_wake_fd  = -1;

static Settings g_active;                     /* what is actually running; updated by live applies */
static const char *g_config_path = SETTINGS_FILE;  /* set from --config in main(); used by reload_apply */

static void log_pending_restart(const char *key, const char *newval, void *ud) {
    (void)newval; (void)ud;
    fprintf(stderr, "pivid-tx: pending restart: %s not applied on reload\n", key);
}

/* Signal handler: AS-async-signal-safe only. eventfd write + relaxed
 * atomic store are both safe; the rest of cleanup happens in main(). */
static void on_signal(int sig) {
    (void)sig;
    __atomic_store_n(&g_shutdown, 1, __ATOMIC_RELAXED);
    if (g_wake_fd >= 0) {
        uint64_t one = 1;
        (void)write(g_wake_fd, &one, sizeof one);
    }
}

/* SIGHUP: request a live reload. AS-async-signal-safe (relaxed store + eventfd
 * write). The apply happens in the main loop, not here. */
static void on_sighup(int sig) {
    (void)sig;
    __atomic_store_n(&g_reload, 1, __ATOMIC_RELAXED);
    if (g_wake_fd >= 0) { uint64_t one = 1; (void)write(g_wake_fd, &one, sizeof one); }
}

static void log_stats(const TX_Dev *dev, const Encoder *e,
                      const struct radio_stats *k,
                      const Settings *s, const MSP_Poll *msp) {
    static uint64_t prev_bytes = 0;
    static uint64_t prev_ms = 0;
    uint64_t now_ms;
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      now_ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull; }
    uint64_t tx_kbps = prev_ms ? kbps_from_delta_counters(prev_bytes, dev->bytes_sent,
                                   (int64_t)(now_ms - prev_ms)) : 0;
    prev_bytes = dev->bytes_sent; prev_ms = now_ms;

    fprintf(stderr,
            "pivid-tx: sent=%llu bytes=%llu dropped_full=%llu dropped_ioctl=%llu"
            " fec_blk=%llu fec_par=%llu oversz=%llu"
            " k_tx=%llu k_btx=%llu spi_err=%llu irq_err=%llu"
            " tx_und=%u wedge_s=%u wedge_h=%u tx_to=%u\n",
            (unsigned long long)dev->packets_sent,
            (unsigned long long)dev->bytes_sent,
            (unsigned long long)dev->dropped_full,
            (unsigned long long)dev->dropped_ioctl,
            (unsigned long long)e->fec.fec_blocks,
            (unsigned long long)e->fec.fec_parity_pkts,
            (unsigned long long)e->fec.oversize_drops,
            (unsigned long long)k->packets_tx,
            (unsigned long long)k->bytes_tx,
            (unsigned long long)k->errors_spi,
            (unsigned long long)k->errors_irq,
            k->tx_fifo_underflow,
            k->wedge_soft, k->wedge_hard,
            k->tx_timeouts_consecutive);

    /* RAM-backed snapshot (tmpfs); best-effort -- a write failure must not
     * disturb streaming. Keys are the OSD/telemetry contract. */
    StatusKV kv[] = {
        { "packets_sent",   dev->packets_sent },
        { "bytes_sent",     dev->bytes_sent },
        { "tx_kbps",        tx_kbps },
        { "dropped_full",   dev->dropped_full },
        { "dropped_ioctl",  dev->dropped_ioctl },
        { "fec_blk",        e->fec.fec_blocks },
        { "fec_par",        e->fec.fec_parity_pkts },
        { "fec_oversize",   e->fec.oversize_drops },
        { "k_packets_tx",   k->packets_tx },
        { "k_bytes_tx",     k->bytes_tx },
        { "k_errors_spi",   k->errors_spi },
        { "k_errors_irq",   k->errors_irq },
        { "k_tx_fifo_underflow", k->tx_fifo_underflow },
        { "k_wedge_soft",   k->wedge_soft },
        { "k_wedge_hard",   k->wedge_hard },
        { "k_tx_timeouts",  k->tx_timeouts_consecutive },
        { "tx_inj_drop",    e->tx_injected_drops },
        { "tx_push_retries", e->tx_push_retries },
        { "sps_reinject",   e->sps_reinject.reinjections },
        /* MSP telemetry slots (appended only when msp_enabled; see n below).
         * fc_link_state is the numeric state code 0=OK 1=INIT 2=TIMEOUT
         * 3=UART_ERROR -- StatusKV renders unsigned decimal, so the dashboard
         * panel maps the number to a label. Keys are stable. */
        { "msp_fc_link_state", 0 },
        { "msp_polls_ok",      0 },
        { "msp_parse_resets",  0 },
        { "msp_crc_failures",  0 },
        { "msp_kf_frags_tx",   0 },
    };
    size_t n = (sizeof(kv) / sizeof(kv[0])) - 5;   /* drop MSP slots by default */
    if (s->msp_enabled && msp) {
        int st = atomic_load(&msp->fc_link_state);
        /* Clamp out-of-range states to an "unknown" sentinel rather than masking
         * (a bare & 3 would wrap a future 5th state to 0 == OK). */
        unsigned long long st_out = (st >= 0 && st < FC_LINK_STATE_COUNT)
                                        ? (unsigned long long)st
                                        : (unsigned long long)FC_LINK_STATE_UNKNOWN;
        kv[n + 0].val = st_out;
        kv[n + 1].val = (unsigned long long)atomic_load(&msp->polls_ok);
        kv[n + 2].val = (unsigned long long)atomic_load(&msp->parse_resets);
        kv[n + 3].val = (unsigned long long)atomic_load(&msp->crc_failures);
        kv[n + 4].val = (unsigned long long)atomic_load(&msp->kf_frags_tx);
        n += 5;
    }
    if (status_write("/run/pivid/tx.status", kv, n) < 0) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "pivid-tx: status_write failed (%s); "
                            "continuing without snapshot file\n",
                    strerror(errno));
        }
    }
    if (e->aqm.mode != AQM_OFF) {
        fprintf(stderr,
            "  aqm: mode=enforce recipe=%d lvl=%d seen=%llu "
            "drop[idr/ref/disp]=%llu/%llu/%llu nal_suppressed=%llu\n",
            (int)e->aqm.recipe, (int)e->aqm.level,
            (unsigned long long)e->aqm.au_seen,
            (unsigned long long)e->aqm.au_dropped[AQM_CLASS_IDR],
            (unsigned long long)e->aqm.au_dropped[AQM_CLASS_REF],
            (unsigned long long)e->aqm.au_dropped[AQM_CLASS_DISP],
            (unsigned long long)e->aqm.nal_suppressed);
    }
}

/* Re-read pivid.conf and apply the SAFE live subset on our own fd: sync word
 * and FEC m/k. FLRC is NOT applied live -- a live SET_FLRC_CONFIG mid-stream
 * wedges the LR2021 framing (HW-verified 2026-06-16; only a module reload
 * clears it). FLRC changes now take effect via modprobe.d/lr2021.conf at the
 * next module reload (pivid-config-agent's TIER_MODRELOAD). Best-effort: a
 * failed ioctl is logged, streaming continues. */
static void reload_apply(TX_Dev *dev, Encoder *enc) {
    Settings nv; char err[128] = {0};
    if (settings_load_checked(&nv, g_config_path, err, sizeof err) != 0 ||
        settings_validate(&nv, err, sizeof err) != 0) {
        fprintf(stderr, "pivid-tx: reload rejected (%s); keeping running config\n", err);
        return;                              /* keep-old: stream undisturbed */
    }

    /* LIVE: sync word (derived from the binding phrase). Safe to apply live;
     * FLRC is NOT (it wedges the chip -- handled by module reload elsewhere). */
    uint32_t sw = settings_get_sync_word(&nv);
    if (sw != settings_get_sync_word(&g_active)) {
        if (tx_set_sync_word(dev, sw) < 0)
            fprintf(stderr, "pivid-tx: reload SET_SYNC_WORD failed (%s)\n", strerror(errno));
    }
    /* LIVE: FEC parity/k. */
    if (nv.fec_m_anchor != g_active.fec_m_anchor ||
        nv.fec_m_ref    != g_active.fec_m_ref    ||
        nv.fec_k        != g_active.fec_k)
        encoder_set_fec(enc, nv.fec_m_anchor, nv.fec_m_ref, nv.fec_k);

    /* Commit the live-applied fields into g_active FIRST, so the restart-tier
     * pending-log below does NOT falsely report binding_phrase (whose effect,
     * the sync word, we just applied live). g_active now matches what runs. */
    strncpy(g_active.binding_phrase, nv.binding_phrase, sizeof g_active.binding_phrase - 1);
    g_active.binding_phrase[sizeof g_active.binding_phrase - 1] = '\0';
    g_active.sync_word    = sw;
    g_active.fec_m_anchor = nv.fec_m_anchor;
    g_active.fec_m_ref    = nv.fec_m_ref;
    g_active.fec_k        = nv.fec_k;

    /* Log any changed RESTART-tier key as pending (left at the running value in
     * g_active -- NOT applied, so in-memory never diverges from hardware). */
    settings_diff_by_tier(&g_active, &nv, TIER_RESTART, log_pending_restart, NULL);

    fprintf(stderr, "pivid-tx: reload applied sync=0x%08X fec(anchor=%d ref=%d k=%d)\n",
            sw, nv.fec_m_anchor, nv.fec_m_ref, nv.fec_k);
}

#ifndef BUILD_VERSION
#define BUILD_VERSION "unknown"   /* Makefile injects -DBUILD_VERSION=<git hash> */
#endif

int main(int argc, char **argv) {
    CliArgs args;
    if (cli_parse(argc, argv, SETTINGS_FILE, &args) != 0) {
        cli_print_usage(stderr, "pivid-tx");
        return 2;
    }
    if (args.show_help)    { cli_print_usage(stdout, "pivid-tx"); return 0; }
    if (args.show_version) { printf("pivid-tx %s\n%s\n", BUILD_VERSION, PIVID_ATTRIBUTION); return 0; }
    if (args.check) {
        char err[128];
        if (settings_check_file(args.config_path, err, sizeof err) != 0) {
            fprintf(stderr, "pivid-tx: config invalid: %s\n", err);
            return 1;
        }
        printf("pivid-tx: config OK\n");
        return 0;
    }
    g_config_path = args.config_path;   /* SIGHUP reload honors --config */

    fprintf(stderr, "pivid-tx: build=%s\n", BUILD_VERSION);
    /* Pin all current + future pages in RAM. Paired with AUTO_SETUP_SWAPFILE_SIZE=0
     * on the deploy image: with swap off and pages locked, a streaming-time page
     * fault that would land on the SD card cannot happen. Soft-fail (warn + carry
     * on) so dev runs without CAP_IPC_LOCK / RLIMIT_MEMLOCK still work. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "pivid-tx: mlockall failed (%s); continuing unlocked\n",
                strerror(errno));
    }

    /* --- Settings + GPIO + device --- */
    Settings settings;
    char err[128];
    if (settings_load_checked(&settings, args.config_path, err, sizeof err) != 0 ||
        settings_validate(&settings, err, sizeof err) != 0) {
        fprintf(stderr, "pivid-tx: config invalid: %s\n", err);
        return 1;
    }
    g_active = settings;   /* baseline "running" config; live applies update it */

    /* tmpfs snapshot dir for the heartbeat status file. mkdir is idempotent;
     * EEXIST is fine. /run is tmpfs so this costs no flash. */
    if (mkdir("/run/pivid", 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "pivid-tx: mkdir /run/pivid failed (%s); "
                        "status file disabled\n", strerror(errno));
    }

    GPIO_Handle gpio;
    if (gpio_init(&gpio, GPIO_LINE_ACTIVITY) < 0) {
        fprintf(stderr, "pivid-tx: gpio_init failed\n");
        return 1;
    }
    gpio_set_tx_led(&gpio, 0);

    TX_Dev dev;
    if (tx_open(&dev, settings_get_sync_word(&settings)) < 0) {
        gpio_close(&gpio);
        return 1;
    }

    /* --- wake_fd + signal handlers ---
     * Set g_wake_fd BEFORE installing signal handlers so a signal arriving
     * mid-init has a valid fd to write to. */
    g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_wake_fd < 0) {
        perror("eventfd");
        tx_close(&dev);
        gpio_close(&gpio);
        return 1;
    }

    struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &(struct sigaction){ .sa_handler = on_sighup, .sa_flags = SA_RESTART }, NULL);
    /* Ignore SIGPIPE via sigaction so the disposition is fully specified. The
     * camera pipe never SIGPIPEs us (we read it; we close the read-end only
     * after the child is reaped), but a stray write elsewhere shouldn't kill us. */
    struct sigaction sa_pipe = { .sa_handler = SIG_IGN, .sa_flags = 0 };
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);

    /* --- Encoder + GPIO thread --- */
    Encoder enc;
    EncoderConfig ec = {
        .fec_m_anchor    = settings.fec_m_anchor,
        .fec_m_ref       = settings.fec_m_ref,
        .fec_k           = settings.fec_k,
        .sps_reinject_ms = settings.sps_reinject_ms,
    };
    ec.aqm.mode   = (strcmp(settings.aqm, "enforce") == 0) ? AQM_ENFORCE : AQM_OFF;
    ec.aqm.recipe = aqm_recipe_from_encoder(settings.recipe);

    /* Spawn rpicam-vid ourselves (was: run-tx.sh `rpicam-vid | pivid-tx`). Fork
     * from the main thread so PR_SET_PDEATHSIG tracks a thread that lives the
     * whole process. A fatal spawn failure here is unrecoverable -> exit. */
    CameraSpawn cam;
    if (camera_spawn_start(&cam, &settings) != 0) {
        fprintf(stderr, "pivid-tx: camera spawn failed; exiting\n");
        close(g_wake_fd);
        tx_close(&dev);
        gpio_close(&gpio);
        return 1;
    }
    encoder_init(&enc, &dev, cam.read_fd, g_wake_fd, &ec);

    GPIO_Thread gt;
    if (gpio_thread_start(&gt, &gpio, &enc) < 0) {
        perror("gpio_thread_start");
        close(g_wake_fd);
        tx_close(&dev);
        gpio_close(&gpio);
        return 1;
    }

    /* --- MSP telemetry poller ---
     * Spawn after tx_open (it shares dev's mutex-protected tx_push) and after
     * the GPIO thread (mirrors that spawn pattern). msp_poll_start memsets
     * internally and only sets thread_started=true on success, so a failed
     * start leaves msp safe for msp_poll_stop. When msp_enabled is false the
     * start is a no-op; we still zero msp so msp_poll_stop is a safe no-op. */
    MSP_Poll msp;
    if (settings.msp_enabled) {
        if (msp_poll_start(&msp, &dev, &settings) < 0) {
            fprintf(stderr, "pivid-tx: msp_poll_start failed; "
                            "continuing without telemetry\n");
        }
    } else {
        memset(&msp, 0, sizeof msp);   /* thread_started stays false */
    }

    /* --- AQM baseline instrumenter (tx_sample) ---
     * Dormant unless PIVID_TX_SAMPLE is set: tx_sample_start still wires
     * dev.sample (the tx_push hook no-ops) so the default path is unchanged.
     * Spawned last so tx_ring + producer_lock are live. A failed start is
     * non-fatal -- it's a debug tool, never a reason to drop the video link. */
    TxSample txsample;
    (void)tx_sample_start(&txsample, &dev);

    fprintf(stderr, "pivid-tx: started, sync_word=0x%08X\n",
            settings_get_sync_word(&settings));

    /* Pidfile so the config agent can SIGHUP us directly: it resolves
     * /run/pivid/<unit>.pid (op_reload_daemon), the same convention pivid-rx /
     * pivid-caster use. /run is tmpfs -> no flash. */
    {
        FILE *pf = fopen("/run/pivid/pivid-tx.pid", "w");
        if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    }

    /* --- Main loop ---
     * encoder_run returns when something needs main's attention:
     *   3 -> 5 s heartbeat due (log + reset)
     *   2 -> wake_fd: shutdown (SIGINT/SIGTERM) OR SIGHUP live reload
     *   1 -> camera pipe EOF (rpicam-vid died -> reap + respawn)
     *  -1 -> unrecoverable error */
    while (!__atomic_load_n(&g_shutdown, __ATOMIC_RELAXED)) {
        int rc = encoder_run(&enc);
        if (rc == 1) {
            /* Camera pipe EOF: rpicam-vid died. Semantic change from the old
             * `stdin EOF = clean exit`: reap + respawn with bounded backoff and
             * re-point the encoder; only g_shutdown (SIGINT/SIGTERM) exits. */
            if (__atomic_load_n(&g_shutdown, __ATOMIC_RELAXED)) break;
            (void)camera_spawn_reaped(&cam);
            if (__atomic_load_n(&g_shutdown, __ATOMIC_RELAXED)) break;
            if (camera_spawn_respawn(&cam) != 0) {
                fprintf(stderr, "pivid-tx: camera respawn failed; exiting\n");
                break;
            }
            encoder_set_input_fd(&enc, cam.read_fd);
            continue;
        }
        if (rc < 0) {
            perror("encoder_run");
            break;
        }

        if (rc == 3) {
            struct radio_stats kstats;
            (void)tx_get_stats(&dev, &kstats);   /* zeroed on failure */
            log_stats(&dev, &enc, &kstats, &settings, &msp);
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            enc.last_stats_ms = (uint64_t)ts.tv_sec * 1000ull
                              + (uint64_t)ts.tv_nsec / 1000000ull;
            continue;
        }

        /* rc == 2: wake_fd was signaled. Two wake sources: SIGINT/SIGTERM
         * (g_shutdown -> exit) and SIGHUP (g_reload -> reload_apply below).
         * The config button moved to pivid-config-agent. */
        if (__atomic_load_n(&g_shutdown, __ATOMIC_RELAXED)) break;

        if (__atomic_load_n(&g_reload, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_reload, 0, __ATOMIC_RELAXED);
            fprintf(stderr, "pivid-tx: SIGHUP reload\n");
            reload_apply(&dev, &enc);
        }
    }

    /* --- Shutdown ---
     * SIGTERM the camera child FIRST so libcamera releases V4L2 cleanly while
     * the rest of teardown (thread joins, etc.) consumes its exit latency. */
    camera_spawn_terminate(&cam);

    /* Stop the GPIO thread FIRST so it's never running over freed state.
     * Then turn LEDs off (now the handle is back in main's ownership). */
    gpio_thread_stop(&gt);
    gpio_set_tx_led(&gpio, 0);
    /* Stop the MSP poller AFTER the encoder loop returned and BEFORE tx_close:
     * the poller pushes via dev's producer_lock mutex, which tx_close destroys.
     * msp_poll_stop signals stop + joins from main's normal path (no
     * async-signal-safety concerns). No-op if msp was never started. */
    msp_poll_stop(&msp);
    /* Stop the sampler before tx_close: its thread reads dev.tx_ring, which
     * tx_close unmaps. No-op if dormant. */
    tx_sample_stop(&txsample, &dev);
    /* Stop AQM (release the shared CSV ref, clear dev.aqm) alongside the
     * sampler, before tx_close unmaps the ring. No-op if dormant. */
    aqm_stop(&enc.aqm, &dev);
    unlink("/run/pivid/pivid-tx.pid");

    fprintf(stderr, "pivid-tx: exit\n");
    {
        struct radio_stats kstats;
        (void)tx_get_stats(&dev, &kstats);   /* fd still open here */
        log_stats(&dev, &enc, &kstats, &settings, &msp);
    }

    tx_close(&dev);
    gpio_close(&gpio);
    /* Bounded-wait + SIGKILL-on-overrun reap of the camera child, then close the
     * pipe read-end LAST (after the reap) so a flushing child never takes SIGPIPE. */
    camera_spawn_finish(&cam, 2000);
    close(g_wake_fd);
    return 0;
}
