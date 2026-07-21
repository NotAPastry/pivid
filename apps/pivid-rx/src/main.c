/* pivid-rx daemon entry point. Mirror of pivid-tx/src/main.c.
 *
 * Wires RX_Dev + Reasm + Output_Sink + RX_Loop + RX_GPIO_Thread together.
 * On wake_fd: either SIGINT/TERM (g_shutdown -> exit) or SIGHUP (g_reload ->
 * reload_apply re-reads pivid.conf and re-applies the live subset -- sync
 * word only -- mid-stream; FLRC applies via module reload). The RX loop keeps
 * receiving throughout. The
 * config-mode button moved to the always-on pivid-config-agent. Single wake
 * path; eventfd_write is AS-async-signal-safe per POSIX. */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "rx.h"
#include "reasm.h"
#include "fec_rx.h"
#include "output.h"
#include "rx_loop.h"
#include "osd_grid_rx.h"
#include "telem_tx.h"
#include "gpio_thread.h"
#include "status_file.h"
#include "kbps.h"

#include "peripherals.h"
#include "radio_ioctl.h"
#include "settings.h"
#include "loopback.h"
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static _Atomic int g_shutdown = 0;
static _Atomic int g_reload   = 0;
static int         g_wake_fd  = -1;

static const char *g_config_path = SETTINGS_FILE;  /* set from --config in main */

#ifdef LATPROBE
#include "resid_hist.h"
#include "mono_ns.h"
static ResidHist g_s6a_rx_fwd;
static const uint64_t S6A_EDGES[] = {
    100000, 250000, 500000, 1000000, 2000000, 4000000,
    8000000, 16000000, 24000000, 33000000, (uint64_t)-1 };
static volatile sig_atomic_t g_dump_req;
#endif

/* AS-async-signal-safe only: relaxed atomic store + eventfd write. */
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

#ifdef LATPROBE
/* SIGUSR1: request a probe histogram dump. AS-async-signal-safe (only sets a
 * flag + writes the wake eventfd). The dump happens in the main loop, not here. */
static void on_sigusr1(int sig) {
    (void)sig;
    g_dump_req = 1;
    if (g_wake_fd >= 0) { uint64_t one = 1; (void)write(g_wake_fd, &one, sizeof one); }
}
#endif

/* Reasm emit callback: frame the NAL with an Annex-B start code and send it to
 * the loopback video sink (one connected-UDP datagram per NAL). The TX encoder
 * strips `00 00 00 01` prefixes when splitting stdin into NALs, so we synthesise
 * them back here -- the caster forwards verbatim and the player's libavformat
 * udp:// demuxer resyncs on Annex-B start codes. Static buffer is fine: this
 * callback only runs on the RX loop thread. */
static void emit_cb(const uint8_t *payload, size_t len, void *user) {
#ifdef LATPROBE
    uint64_t t0 = pv_mono_ns();
#endif
    static uint8_t framed[4 + REASM_BUF_BYTES];
    Output_Sink *sink = user;
    if (len > REASM_BUF_BYTES) len = REASM_BUF_BYTES;
    framed[0] = 0x00; framed[1] = 0x00; framed[2] = 0x00; framed[3] = 0x01;
    memcpy(framed + 4, payload, len);
    (void)sink->write(sink, framed, len + 4);
#ifdef LATPROBE
    { uint64_t now = pv_mono_ns(); if (now >= t0) resid_hist_add(&g_s6a_rx_fwd, now - t0); }
#endif
}

/* FEC-decode stage emits recovered inner records here; forward to reasm. */
static void fec_to_reasm(const uint8_t *record, size_t len, void *user) {
    reasm_feed((Reasm *)user, record, len);
}

/* Heartbeat status writer. Raw-forward mode only now: the Output_Sink counters
 * plus the OSD-grid receive-health fields the dashboard shows (keyframe complete/
 * dropped counts reuse the msp_rx_ok/err keys for dashboard compatibility).
 * StatusKV values are UNSIGNED DECIMAL only -- signed quantities follow the
 * `*_dbm_neg` convention (negate so the dashboard re-negates). */
static void log_stats(const RX_Dev *dev, const Reasm *r, const RX_Loop *l,
                      const Output_Sink *out, RxGrid *grid,
                      const struct radio_stats *k) {
    static uint64_t prev_bytes = 0;
    static uint64_t prev_ms = 0;
    uint64_t now_ms;
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      now_ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull; }
    uint64_t rx_kbps = prev_ms ? kbps_from_delta_counters(prev_bytes, dev->bytes_received,
                                   (int64_t)(now_ms - prev_ms)) : 0;
    prev_bytes = dev->bytes_received; prev_ms = now_ms;

    /* OSD-grid receive-health gauges. Racy read of the RxGrid reasm counters
     * from the main thread vs the rx_loop writer; benign staleness-tolerant
     * gauge, matching the prior msp_telemetry pattern. RxGrid has no per-frame
     * arrival timestamp, so telem_age_ms keeps the "no telemetry yet" sentinel
     * until a real grid age exists. */
    uint64_t frags_rx, fdone, fdrop, fstale;
    rx_grid_health(grid, &frags_rx, &fdone, &fdrop, &fstale);
    unsigned long long telem_age_ms = 0xFFFFFFFFull;

    fprintf(stderr,
            "pivid-rx: nals=%llu seq_drop=%llu to_drop=%llu ovfl=%llu inv=%llu len_drop=%llu "
            "torn=%llu rssi=%d sink_ok=%llu sink_err=%llu"
            " fec_blk=%llu fec_rec=%llu fec_unrec=%llu"
            " k_rx=%llu k_brx=%llu ring_full=%llu crc=%u crc_drop=%u"
            " sync=%u len=%u fifo_ovf=%u silent_ms=%u inj_drop=%llu"
            " parse_drop=%u\n",
            (unsigned long long)r->nals_emitted,
            (unsigned long long)r->dropped_seq_mismatch,
            (unsigned long long)r->dropped_timeout,
            (unsigned long long)r->dropped_overflow,
            (unsigned long long)r->dropped_invalid,
            (unsigned long long)r->dropped_len_mismatch,
            (unsigned long long)dev->dropped_torn,
            (int)dev->rssi_dbm,
            (unsigned long long)out->writes_ok,
            (unsigned long long)out->writes_failed,
            (unsigned long long)l->fec->fec_blocks,
            (unsigned long long)l->fec->fec_recovered,
            (unsigned long long)l->fec->fec_unrecoverable,
            (unsigned long long)k->packets_rx,
            (unsigned long long)k->bytes_rx,
            (unsigned long long)k->rx_ring_full_drops,
            k->crc, k->crc_drops, k->sync, k->len,
            k->rx_fifo_overflow, k->rx_silent_ms,
            (unsigned long long)l->injected_drops,
            l->fec->drops_parse);

    StatusKV kv[] = {
        { "nals",          r->nals_emitted },
        { "seq_drop",      r->dropped_seq_mismatch },
        { "to_drop",       r->dropped_timeout },
        { "ovfl",          r->dropped_overflow },
        { "invalid",       r->dropped_invalid },
        { "len_drop",      r->dropped_len_mismatch },
        { "torn",          dev->dropped_torn },
        { "packets_received", dev->packets_received },
        { "bytes_received",   dev->bytes_received },
        { "rx_kbps",          rx_kbps },
        { "rssi_dbm_neg",  (unsigned long long)(-(int)dev->rssi_dbm) },
        { "fec_blk",       l->fec->fec_blocks },
        { "fec_rec",       l->fec->fec_recovered },
        { "fec_unrec",     l->fec->fec_unrecoverable },
        { "fec_unrec_short",   l->fec->unrec_short },     /* have<k: bursty erasure loss */
        { "fec_unrec_present", l->fec->unrec_present },   /* k present, decode failed: corrupt symbol (IMP-5) */
        { "fec_unrec_defsum",  l->fec->unrec_deficit },   /* sum(k-have) over short blocks */
        { "fec_late_emit",     l->fec->late_after_emit }, /* IMP-4: late symbols after early-emit (benign, ~m/clean block) */
        { "k_packets_rx",  k->packets_rx },
        { "k_bytes_rx",    k->bytes_rx },
        { "k_ring_full",   k->rx_ring_full_drops },
        { "k_crc",         k->crc },
        { "k_crc_drops",   k->crc_drops },
        { "k_sync",        k->sync },
        { "k_len",         k->len },
        { "k_tmout",       k->tmout },
        { "k_rx_fifo_overflow", k->rx_fifo_overflow },
        { "k_wedge_soft",  k->wedge_soft },
        { "k_wedge_hard",  k->wedge_hard },
        { "k_rx_silent_ms", k->rx_silent_ms },
        { "inj_drop",      l->injected_drops },
        { "bid_noninc",    l->fec->bid_noninc },
        { "fec_innerlen",  l->fec->drops_inner_len },
        { "sink_ok",       out->writes_ok },
        { "sink_err",      out->writes_failed },
        { "telem_age_ms",  telem_age_ms },
        { "msp_rx_ok",     (unsigned long long)fdone },          /* completed keyframes */
        { "msp_rx_err",    (unsigned long long)(fdrop + fstale) }, /* dropped + stale frags */
        { "osd_frags_rx",     (unsigned long long)frags_rx },
        { "osd_frames_done",  (unsigned long long)fdone },
        { "osd_frames_dropped", (unsigned long long)fdrop },
        { "osd_frags_stale",  (unsigned long long)fstale },
        { "vid_rssi_dbm_neg", (unsigned long long)(-(int)rx_grid_vid_rssi(grid)) },
        { "vid_lq_pct",    (unsigned long long)rx_grid_vid_lq(grid) },
    };
    size_t n = sizeof(kv) / sizeof(kv[0]);

    if (status_write("/run/pivid/rx.status", kv, n) < 0) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "pivid-rx: status_write failed (%s); "
                            "continuing without snapshot file\n",
                    strerror(errno));
        }
    }
}

/* Re-read pivid.conf and apply the SAFE live subset: sync word + OSD injection
 * cell positions. FLRC is NOT applied live (a live SET_FLRC_CONFIG wedges the
 * chip; HW-verified 2026-06-16) -- it takes effect via modprobe.d at the next
 * module reload. RX FEC is param-less. The OSD_VID_*_CELL keys are TIER_LIVE so
 * the dashboard relocates the injected RSSI/LQ cells without an RX restart. */
static void reload_apply(RX_Dev *dev, RxGrid *grid) {
    Settings s; char err[128] = {0};
    /* Checked load + validate, mirroring pivid-tx reload_apply: a config that
     * fails to load (unreadable) or validate is REJECTED and the running config
     * kept, instead of silently applying clamped/garbage values (e.g. a wrong
     * sync word that would drop the link). The atomic conf writer normally
     * prevents partial reads; this also covers a hand-edited/corrupt conf. */
    if (settings_load_checked(&s, g_config_path, err, sizeof err) != 0 ||
        settings_validate(&s, err, sizeof err) != 0) {
        fprintf(stderr, "pivid-rx: reload rejected (%s); keeping running config\n", err);
        return;                              /* keep-old: sync word + OSD cells undisturbed */
    }
    uint32_t sw = settings_get_sync_word(&s);
    if (rx_set_sync_word(dev, sw) < 0)
        fprintf(stderr, "pivid-rx: reload SET_SYNC_WORD failed (%s)\n", strerror(errno));
    /* settings clamps row 0..15, col 0..29 so the (uint8_t) casts are safe. */
    rx_grid_set_cells(grid,
                      (uint8_t)s.osd_vid_rssi_row, (uint8_t)s.osd_vid_rssi_col,
                      (uint8_t)s.osd_vid_lq_row,   (uint8_t)s.osd_vid_lq_col);
    fprintf(stderr, "pivid-rx: reload applied sync=0x%08X, OSD cells RSSI=%d,%d LQ=%d,%d "
                    "(flrc via module reload only)\n", sw,
            s.osd_vid_rssi_row, s.osd_vid_rssi_col, s.osd_vid_lq_row, s.osd_vid_lq_col);
}

#ifndef BUILD_VERSION
#define BUILD_VERSION "unknown"   /* Makefile injects -DBUILD_VERSION=<git hash> */
#endif

/* RSSI calibration retry. The LR2021 power-on firmware calibration (ADC offset /
 * self-reception-cancellation) is corrupted by the incoming TX signal present
 * during its boot calibration window, leaving the reported RSSI ~30 dB low on
 * most boots (reception is unaffected -- it is a gauge error). Only a module
 * reload (hardware NRESET) re-rolls the cal, landing good stochastically. The
 * daemon detects a bad cal via RADIO_IOCTL_GET_CAL_DIFF (RSSI at a forced LOW vs
 * HIGH gain step: input-referred so a good cal reads the same, a bad cal diverges
 * ~15 dB) and reloads the module until it passes -- all in-process, no wrapper. */
#define RX_CAL_THRESH_DB    8     /* gain-consistency >= this dB => bad cal */
#define RX_CAL_MAX_RELOADS  12    /* give up after this many reloads (gauge only) */

static int run_modprobe(int remove_first) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (remove_first) execlp("modprobe", "modprobe", "-r", "lr2021", (char *)NULL);
        else              execlp("modprobe", "modprobe", "lr2021", (char *)NULL);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* NRESET the chip by unloading + reloading the driver, then wait for the device
 * node to reappear. The caller MUST have closed /dev/radio0 first (modprobe -r
 * fails while the chardev is open). */
static void reload_radio_module(void) {
    run_modprobe(1);                                  /* modprobe -r lr2021 */
    run_modprobe(0);                                  /* modprobe lr2021    */
    for (int i = 0; i < 30 && access("/dev/radio0", F_OK) != 0; i++)
        usleep(200000);                               /* up to ~6 s */
}

/* Open the radio and ensure a good RSSI calibration. Reloads the module (the
 * only thing that re-rolls the POR cal) until the gain-consistency probe passes,
 * close->reload->reopen each time. Leaves *dev OPEN on return. Returns 0, or -1
 * only if the device cannot be opened at all. */
static int rx_open_with_good_cal(RX_Dev *dev, uint32_t sync_word) {
    int reloads = 0;
    for (;;) {
        if (rx_open(dev, sync_word) < 0)
            return -1;
        /* Wait for the link to (re)acquire after a reload -- RSSI goes non-zero
         * once packets flow -- then let the AGC settle before probing; measuring
         * too early reads a false "good". No signal at all means no RF was present
         * during cal, which is exactly the case that calibrates correctly. */
        for (int w = 0; w < 60; w++) {
            int8_t r = 0;
            if (rx_get_rssi(dev, &r) == 0 && r != 0) break;
            usleep(100000);                           /* up to ~6 s */
        }
        sleep(3);                                     /* AGC settle into its park */

        int32_t cal_db = 0;
        int measured = (rx_get_cal_diff(dev, &cal_db) == 0);
        if (!measured || cal_db < RX_CAL_THRESH_DB) {
            fprintf(stderr, "pivid-rx: RSSI cal good (gain-consistency=%d dB, "
                            "%d reload%s)\n", cal_db, reloads, reloads == 1 ? "" : "s");
            return 0;                                 /* keep dev open */
        }
        if (reloads >= RX_CAL_MAX_RELOADS) {
            fprintf(stderr, "pivid-rx: RSSI cal still off (%d dB) after %d reloads "
                            "-- continuing (gauge only, video unaffected)\n",
                    cal_db, reloads);
            return 0;                                 /* give up; keep dev open */
        }
        rx_close(dev);                                /* close so modprobe -r can unload */
        reloads++;
        fprintf(stderr, "pivid-rx: RSSI cal bad (%d dB), reloading lr2021 (%d/%d)\n",
                cal_db, reloads, RX_CAL_MAX_RELOADS);
        reload_radio_module();
    }
}

int main(int argc, char **argv) {
    CliArgs args;
    if (cli_parse(argc, argv, SETTINGS_FILE, &args) != 0) {
        cli_print_usage(stderr, "pivid-rx");
        return 2;
    }
    if (args.show_help)    { cli_print_usage(stdout, "pivid-rx"); return 0; }
    if (args.show_version) { printf("pivid-rx %s\n", BUILD_VERSION); return 0; }
    if (args.check) {
        char err[128];
        if (settings_check_file(args.config_path, err, sizeof err) != 0) {
            fprintf(stderr, "pivid-rx: config invalid: %s\n", err);
            return 1;
        }
        printf("pivid-rx: config OK\n");
        return 0;
    }
    g_config_path = args.config_path;   /* SIGHUP reload honors --config */

    fprintf(stderr, "pivid-rx: build=%s\n", BUILD_VERSION);
    /* Pin all current + future pages in RAM. Paired with AUTO_SETUP_SWAPFILE_SIZE=0
     * on the deploy image: with swap off and pages locked, a streaming-time page
     * fault that would land on the SD card cannot happen. Soft-fail (warn + carry
     * on) so dev runs without CAP_IPC_LOCK / RLIMIT_MEMLOCK still work. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "pivid-rx: mlockall failed (%s); continuing unlocked\n",
                strerror(errno));
    }

    /* --- Settings + GPIO + device --- */
    Settings settings;
    char err[128];
    if (settings_load_checked(&settings, args.config_path, err, sizeof err) != 0 ||
        settings_validate(&settings, err, sizeof err) != 0) {
        fprintf(stderr, "pivid-rx: config invalid: %s\n", err);
        return 1;
    }

    if (mkdir("/run/pivid", 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "pivid-rx: mkdir /run/pivid failed (%s); "
                        "status file disabled\n", strerror(errno));
    }

    GPIO_Handle gpio;
    if (gpio_init(&gpio, GPIO_LINE_ACTIVITY) < 0) {
        fprintf(stderr, "pivid-rx: gpio_init failed\n");
        return 1;
    }

    RX_Dev dev;
    if (rx_open_with_good_cal(&dev, settings_get_sync_word(&settings)) < 0) {
        gpio_close(&gpio);
        return 1;
    }

    /* OSD-grid seqlock store: the RX loop's TYPE=1 dispatch reassembles BF
     * DisplayPort keyframes into it (with vid RSSI/LQ injected on the reserved
     * cells); the telem_tx sender reads it lock-free and ships the full grid wire
     * to the caster. RSSI cell row0/col0, LQ cell row1/col0 --
     * wires these from settings (OSD_VID_RSSI_CELL / OSD_VID_LQ_CELL).
     * settings clamps row 0..15, col 0..29 so the (uint8_t) casts are safe. */
    RxGrid grid;
    rx_grid_init(&grid,
                 (uint8_t)settings.osd_vid_rssi_row, (uint8_t)settings.osd_vid_rssi_col,
                 (uint8_t)settings.osd_vid_lq_row,   (uint8_t)settings.osd_vid_lq_col);

    /* Single output: raw Annex-B NALs, one per UDP datagram, to the co-located
     * pivid-caster on loopback. No remote address config -- the caster fans out. */
    Output_Sink sink;
    if (output_udp_open(&sink, LOOPBACK_ADDR, LOOPBACK_VIDEO_PORT) < 0) {
        fprintf(stderr, "pivid-rx: failed to open loopback video sink\n");
        rx_close(&dev);
        gpio_close(&gpio);
        return 1;
    }

    Reasm r;
    reasm_init(&r, emit_cb, &sink);

    /* static (BSS, not main()'s stack): ~235 KB of slot buffers; fec_rx_init
     * zeroes it so static costs nothing. Matches the `static framed[]` above. */
    static FecRx fec;
    fec_rx_init(&fec, fec_to_reasm, &r);

    /* Set g_wake_fd BEFORE installing signal handlers so a signal arriving
     * mid-init has a valid fd to write to. */
    g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_wake_fd < 0) {
        perror("eventfd");
        sink.close(&sink);
        rx_close(&dev);
        gpio_close(&gpio);
        return 1;
    }

    struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &(struct sigaction){ .sa_handler = on_sighup, .sa_flags = SA_RESTART }, NULL);
#ifdef LATPROBE
    sigaction(SIGUSR1, &(struct sigaction){ .sa_handler = on_sigusr1, .sa_flags = SA_RESTART }, NULL);
    resid_hist_init(&g_s6a_rx_fwd, "s6a_rx_fwd", S6A_EDGES,
                    (int)(sizeof S6A_EDGES / sizeof S6A_EDGES[0]), 2000000ull);
#endif
    /* Ignore SIGPIPE so a transient socket error never kills the daemon. */
    struct sigaction sa_pipe = { .sa_handler = SIG_IGN, .sa_flags = 0 };
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);

    RX_Loop loop;
    rx_loop_init(&loop, &dev, &r, &fec, &sink, g_wake_fd);
    loop.grid = &grid;

    RX_GPIO_Thread gt;
    if (rx_gpio_thread_start(&gt, &gpio, &loop.last_valid_rx_ns) < 0) {
        perror("rx_gpio_thread_start");
        close(g_wake_fd);
        sink.close(&sink);
        rx_close(&dev);
        gpio_close(&gpio);
        return 1;
    }

    /* Telemetry sender: ships the OSD grid to the caster at 20 Hz. */
    TelemTx ttx;
    if (telem_tx_start(&ttx, &grid) < 0) {
        fprintf(stderr, "pivid-rx: telem_tx_start failed\n");
        rx_gpio_thread_stop(&gt);
        close(g_wake_fd);
        sink.close(&sink);
        rx_close(&dev);
        gpio_close(&gpio);
        return 1;
    }

    fprintf(stderr, "pivid-rx: started, sync_word=0x%08X (raw-forward -> %s:%d)\n",
            settings_get_sync_word(&settings),
            LOOPBACK_ADDR, LOOPBACK_VIDEO_PORT);

    /* Pidfile so the config agent can SIGHUP us directly by exact PID rather
     * than pattern-matching the process table. /run is tmpfs -> no flash. */
    {
        FILE *pf = fopen("/run/pivid/pivid-rx.pid", "w");
        if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    }

    /* --- Main loop ---
     * rx_loop_run returns when something needs main's attention:
     *   3 -> 5 s heartbeat due (refresh RSSI + log + reset)
     *   2 -> wake_fd: shutdown (SIGINT/SIGTERM) OR SIGHUP live reload
     *   1 -> /dev/radio0 unrecoverable (POLLERR/POLLHUP)
     *  -1 -> poll() failed */
    while (!__atomic_load_n(&g_shutdown, __ATOMIC_RELAXED)) {
        int rc = rx_loop_run(&loop);
        if (rc == 1) break;                        /* unrecoverable */
        if (rc < 0) {
            perror("rx_loop_run");
            break;
        }

        if (rc == 3) {
            /* Heartbeat tick. RSSI ioctl is best-effort: a failure here
             * (radio busy / chip glitch) shouldn't take down the daemon. */
            rx_get_rssi(&dev, NULL);
            /* Surface the radio's video-link RSSI to telemetry/OSD (best-effort;
             * rx_get_rssi leaves dev.rssi_dbm at its last value on failure). */
            rx_grid_set_vid_rssi(&grid, dev.rssi_dbm);
            struct radio_stats kstats;
            (void)rx_get_stats(&dev, &kstats);   /* zeroed on failure */
            log_stats(&dev, &r, &loop, &sink, &grid, &kstats);
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            loop.last_stats_ms = (uint64_t)ts.tv_sec * 1000ull
                               + (uint64_t)ts.tv_nsec / 1000000ull;
            continue;
        }

        /* rc == 2: wake_fd was signaled. Two wake sources: SIGINT/SIGTERM
         * (g_shutdown -> exit) and SIGHUP (g_reload -> reload_apply below).
         * The config button moved to pivid-config-agent. */
#ifdef LATPROBE
        /* A LATPROBE build adds a third wake source: SIGUSR1 -> g_dump_req
         * (histogram dump below). */
#endif
        if (__atomic_load_n(&g_shutdown, __ATOMIC_RELAXED)) break;

#ifdef LATPROBE
        if (g_dump_req) {
            g_dump_req = 0;
            fec_rx_probe_dump(STDERR_FILENO);
            resid_hist_dump(&g_s6a_rx_fwd, STDERR_FILENO);
        }
#endif

        if (__atomic_load_n(&g_reload, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_reload, 0, __ATOMIC_RELAXED);
            fprintf(stderr, "pivid-rx: SIGHUP reload\n");
            reload_apply(&dev, &grid);
        }
    }

    /* --- Shutdown ---
     * Stop the GPIO thread FIRST so it's never running over freed state.
     * Then turn the bind LED off (handle is back in main's ownership).
     * Log stats BEFORE we tear down sinks / device so the counters reflect
     * a coherent view rather than a half-closed one. */
    rx_gpio_thread_stop(&gt);
    telem_tx_stop(&ttx);
    unlink("/run/pivid/pivid-rx.pid");

    fprintf(stderr, "pivid-rx: exit\n");
    {
        struct radio_stats kstats;
        (void)rx_get_stats(&dev, &kstats);
        log_stats(&dev, &r, &loop, &sink, &grid, &kstats);
    }

    sink.close(&sink);
    rx_close(&dev);
    gpio_close(&gpio);
    close(g_wake_fd);
    return 0;
}
