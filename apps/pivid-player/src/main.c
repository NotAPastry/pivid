#define _POSIX_C_SOURCE 200809L
/* apps/pivid-player/src/main.c -- pivid-player desktop viewer.
 *
 * Spawns the decode, telemetry-recv and control-client threads, then runs the
 * SDL render loop on the main thread (SDL must pump events on the main thread on
 * macOS). Each frame: read the latest video frame + MSP OSD grid (latest-wins,
 * never blocking), derive the connection state, and present. On quit, stop the
 * threads (control client sends UNSUBSCRIBE). */
#include "player_args.h"
#include "player_state.h"
#include "frame_slot.h"
#include "telem_slot.h"
#include "video_decode.h"
#include "telem_recv.h"
#include "control_client.h"
#include "mdns_browse.h"
#include "render.h"
#include "control.h"       /* CASTER_CTRL_PORT */
#include "osd_grid.h"
#include "player_clock.h"

#include <libavutil/frame.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BUILD_VERSION
#define BUILD_VERSION "unknown"   /* CMake injects -DBUILD_VERSION="<git hash>" */
#endif

#ifdef LATPROBE
#include <unistd.h>        /* STDERR_FILENO */
#include "resid_hist.h"
#include "player_clock.h"
static ResidHist g_s8_present;
static const uint64_t S8_EDGES[] = {
    100000, 250000, 500000, 1000000, 2000000, 4000000,
    8000000, 16000000, 24000000, 33000000, (uint64_t)-1 };
static uint64_t g_s8_last_gen;
#endif

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* --- LAN-drop instrumentation (wire-neutral diagnostic; SIGUSR1 or exit) -----
 * Reads the reassembly/AU/ring drop counters to partition player-internal loss
 * from wire/socket loss during the IDR-burst concealment investigation. Counters
 * are monotonic; the cross-thread read is a benign diagnostic race. */
static volatile sig_atomic_t g_dump_stats = 0;
static void on_usr1(int sig) { (void)sig; g_dump_stats = 1; }
static void dump_lan_stats(const VideoDecode *v, const char *when) {
    const LanReasm *r = &v->lan_src.reasm;
    const AuAsm    *a = &v->lan_src.auasm;
    unsigned long long complete = (unsigned long long)r->nals_complete;
    unsigned long long partial  = (unsigned long long)r->nals_partial;
    unsigned long long tot_nals = complete + partial;
    double partial_pct = tot_nals ? 100.0 * (double)partial / (double)tot_nals : 0.0;
    fprintf(stderr,
        "pivid-player LAN-stats [%s]: reasm nals_complete=%llu nals_partial=%llu "
        "(%.2f%% truncated) nal_gaps=%llu chunks_dropped=%llu bad_magic=%llu "
        "nal_degenerate=%llu | au nals_in=%llu aus_emitted=%llu overflow_drops=%llu | "
        "ring au_overrun=%llu | frames_decoded=%llu\n",
        when, complete, partial, partial_pct,
        (unsigned long long)r->nal_gaps,
        (unsigned long long)r->chunks_dropped, (unsigned long long)r->bad_magic,
        (unsigned long long)r->nal_degenerate,
        (unsigned long long)a->nals_in, (unsigned long long)a->aus_emitted,
        (unsigned long long)a->overflow_drops,
        (unsigned long long)v->lan_src.au_overrun,
        (unsigned long long)atomic_load(&((VideoDecode *)v)->frames_decoded));
#ifdef LATPROBE
    video_decode_probe_dump(STDERR_FILENO);
    resid_hist_dump(&g_s8_present, STDERR_FILENO);
#endif
}

/* Cap the render/present rate. The SDL renderer requests PRESENTVSYNC, but the
 * KMSDRM backend (headless RX Pi) does not reliably block on vsync, so without
 * an explicit cap the loop busy-spins -- pegging a core AND, critically on the
 * RX Pi, the continuous GPU/KMS display traffic perturbs the bcm2835 SPI-DMA
 * radio read (rx_frame_stray ~1.3% -> 6%+, corrupting reception for EVERY
 * subscriber). 60 Hz is ample for 30 fps video + ~5 Hz OSD. */
#define RENDER_INTERVAL_NS  16666667LL   /* ~60 fps */

int main(int argc, char **argv) {
    PlayerArgs args;
    int rc = player_args_parse(argc, argv, &args);
    if (rc == 1) { fputs(player_args_usage(), stdout); return 0; }     /* --help */
    if (rc != 0) { fputs(player_args_usage(), stderr); return 2; }     /* bad flag */
    if (args.show_version) { printf("pivid-player %s\n%s\n", BUILD_VERSION, PIVID_ATTRIBUTION); return 0; }

    /* Switch-only config: args carry built-in defaults + any CLI overrides. */
    bool hw_decode = args.hw_decode;
    /* --port only matters under explicit --caster (mDNS supplies the port via SRV).
     * 0 => fall back to the well-known default. */
    uint16_t control_port = args.port ? args.port : CASTER_CTRL_PORT;
    /* No --caster is not fatal: auto-discover it via mDNS. */
    bool have_caster = args.caster_ip[0] != '\0';

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_usr1);   /* dump LAN-drop counters (see dump_lan_stats) */

    /* Slots. */
    FrameSlot vslot; frame_slot_init(&vslot);
    TelemSlot tslot; telem_slot_init(&tslot);

    /* video_decode + telem_recv are passive UDP listeners (no caster IP needed) ->
     * start immediately. Only the control client needs the resolved address. */
    /* Both receive sockets bind ephemeral ports (passed 0); the OS-assigned port
     * is read back below and advertised to the caster via the control client. */
    VideoDecode  vdec; TelemRecv trecv; ControlClient cctl;
    if (video_decode_start(&vdec, args.video_port, &vslot, hw_decode) != 0) {
        fprintf(stderr, "failed to start video decode\n"); return 1;
    }
    if (telem_recv_start(&trecv, 0, &tslot) != 0) {
        fprintf(stderr, "failed to start telem recv\n"); video_decode_stop(&vdec); return 1;
    }
    uint16_t video_port = vdec.video_port;   /* actually-bound ephemeral ports */
    uint16_t telem_port = trecv.telem_port;

    /* Window up first so the DISCOVERING ("Searching for caster…") banner can show
     * while we browse. */
    /* Pass raw --width/--height (0 = unset); render_create applies windowed
     * defaults and, in fullscreen, uses an explicit size to pick the scanout
     * mode (lower res = less display memory-bus traffic on the RX Pi). */
    Render *r = render_create(args.win_w, args.win_h, args.fullscreen);
    if (!r) { telem_recv_stop(&trecv); video_decode_stop(&vdec); return 1; }

    /* Explicit caster -> subscribe now. Else browse mDNS and subscribe once a
     * caster is discovered (polled in the render loop below). */
    bool resolved = false, cctl_started = false, browsing = false;
    MdnsBrowser browser;
    if (have_caster) {
        if (control_client_start(&cctl, args.caster_ip, control_port,
                                 video_port, telem_port) != 0) {
            fprintf(stderr, "failed to start control client\n");
            render_destroy(r); telem_recv_stop(&trecv); video_decode_stop(&vdec); return 1;
        }
        resolved = cctl_started = true;
        fprintf(stderr, "pivid-player: using configured caster %s:%u "
                        "(--caster; mDNS discovery skipped)\n",
                args.caster_ip, (unsigned)control_port);
    } else if (mdns_browser_start(&browser, "_pivid._udp") == 0) {
        browsing = true;
        fprintf(stderr, "pivid-player: no caster set -- discovering via mDNS\n");
    } else {
        fprintf(stderr, "pivid-player: mDNS browse failed to start; pass --caster <ip>\n");
        render_destroy(r); telem_recv_stop(&trecv); video_decode_stop(&vdec); return 1;
    }

    AVFrame *vframe = av_frame_alloc();
    bool ever_decoded = false;
    int64_t last_frame_ns = 0;
    uint64_t last_gen = 0;
    const int64_t start_ns = now_ns();   /* for the one-shot acquire-latency log */

    /* Loop/poll rate. Finer than the video rate just lowers the latency between
     * a frame arriving and being presented (present-on-change), reducing display
     * timing jitter without adding latency. Tunable for A/B (PIVID_RENDER_FPS). */
    int64_t render_interval_ns = RENDER_INTERVAL_NS;
    const char *fps_env = getenv("PIVID_RENDER_FPS");
    if (fps_env && atoi(fps_env) > 0) render_interval_ns = 1000000000LL / atoi(fps_env);

    /* Presentation jitter buffer (opt-in: PIVID_PRESENT_BUFFER=N frames, 0=off).
     * On the weak Zero 2 W, SW decode can't hold a steady cadence under motion;
     * queuing N frames and releasing them on a fixed 30fps clock absorbs the
     * decode-time jitter, at the cost of ~N*33ms latency. Off by default keeps
     * the low-latency path for Mac/desktop. */
    #define PRESENT_Q_MAX 6
    #define PRESENT_INTERVAL_NS 33333333LL   /* 30 fps cadence */
    int present_buf = 0;
    const char *pb = getenv("PIVID_PRESENT_BUFFER");
    if (pb && atoi(pb) > 0) { present_buf = atoi(pb); if (present_buf > PRESENT_Q_MAX) present_buf = PRESENT_Q_MAX; }
    AVFrame *pq[PRESENT_Q_MAX]; int pqn = 0;          /* FIFO of queued frames */
    AVFrame *shown = av_frame_alloc();                /* currently displayed frame */
    bool shown_valid = false, primed = false;
    int64_t next_present_ns = 0;

#ifdef LATPROBE
    resid_hist_init(&g_s8_present, "s8_present", S8_EDGES,
                    (int)(sizeof S8_EDGES / sizeof S8_EDGES[0]), 2000000ull);
#endif

    bool running = true;
    while (running && !g_quit) {
        running = render_pump_events(r);
        int64_t t = now_ns();

        if (g_dump_stats) { g_dump_stats = 0; dump_lan_stats(&vdec, "SIGUSR1"); }

        /* Late caster discovery: subscribe the moment mDNS resolves one. */
        if (!resolved && browsing) {
            char dip[16]; uint16_t dport;
            if (mdns_browser_result(&browser, dip, &dport)) {
                resolved = true;
                uint16_t cp = dport ? dport : CASTER_CTRL_PORT;
                if (control_client_start(&cctl, dip, cp,
                                         video_port, telem_port) == 0) {
                    cctl_started = true;
                    fprintf(stderr, "pivid-player: discovered caster %s:%u\n",
                            dip, (unsigned)cp);
                } else {
                    fprintf(stderr, "pivid-player: control client start failed for %s\n", dip);
                }
            }
        }

        /* Pull the latest decoded frame (latest-wins ref into vframe). */
        int64_t fns = 0; uint64_t gen = 0;   /* assigned only if frame_slot_take succeeds */
        const AVFrame *present_frame = NULL;
        bool video_changed = false;
        bool got_new = false, have_frame = false;
        if (frame_slot_take(&vslot, vframe, &fns, &gen)) {
            have_frame = true;
            if (gen != last_gen) {
                if (!ever_decoded)   /* first frame: report time-to-acquire (late-join) */
                    fprintf(stderr, "pivid-player: acquired video after %lld ms\n",
                            (long long)((t - start_ns) / 1000000));
                last_gen = gen; last_frame_ns = fns; ever_decoded = true;
                got_new = true;      /* a newly decoded picture this tick */
            }
        }

        if (present_buf == 0) {
            /* Low-latency path: present the latest frame on change. */
            present_frame = have_frame ? vframe : NULL;
            video_changed = got_new;
        } else {
            /* Jitter-buffered path: enqueue each new frame, release one per
             * fixed 30fps tick from `shown`, priming `present_buf` frames first. */
            if (got_new) {
                AVFrame *nf = av_frame_alloc();
                if (nf && av_frame_ref(nf, vframe) == 0) {
                    if (pqn == PRESENT_Q_MAX) {
                        av_frame_free(&pq[0]);
                        memmove(pq, pq + 1, (PRESENT_Q_MAX - 1) * sizeof *pq); pqn--;
                    }
                    pq[pqn++] = nf;
                } else if (nf) { av_frame_free(&nf); }
            }
            if (!primed && pqn >= present_buf) { primed = true; next_present_ns = t; }
            if (primed && t >= next_present_ns && pqn > 0) {
                av_frame_unref(shown);
                if (av_frame_ref(shown, pq[0]) == 0) shown_valid = true;
                av_frame_free(&pq[0]);
                memmove(pq, pq + 1, (PRESENT_Q_MAX - 1) * sizeof *pq); pqn--;
                video_changed = true;
                next_present_ns += PRESENT_INTERVAL_NS;
                if (t - next_present_ns > PRESENT_INTERVAL_NS) next_present_ns = t; /* resync after a stall */
            }
            present_frame = shown_valid ? shown : NULL;
        }

        /* Latest MSP OSD grid. */
        osd_grid_t grid; int64_t telem_recv_ns;
        telem_slot_read(&tslot, &grid, &telem_recv_ns);
        (void)telem_recv_ns;   /* staleness/warn derivation retired with the value OSD */

        PlayerConnState st = player_state_eval(resolved, ever_decoded, last_frame_ns, t);

        /* When stalled, present_frame is NULL so render clears to black; the grid
         * (from the FC via MSP DisplayPort) is drawn over video or black. */
        render_present(r, present_frame, &grid, st, t, video_changed);
#ifdef LATPROBE
        /* have_frame gates on frame_slot_take() succeeding this tick, so fns/gen
         * are real (never the uninitialized decl values). Exact on the default
         * present_buf==0 path; under PIVID_PRESENT_BUFFER>0 this measures the
         * decoded-frame publish->present, not the queued-release frame (a known
         * skew, since fns/gen track the take, not the release). */
        if (have_frame && (uint64_t)gen != g_s8_last_gen && video_changed) {
            g_s8_last_gen = (uint64_t)gen;
            int64_t now = now_ns();
            if (now >= fns) resid_hist_add(&g_s8_present, (uint64_t)(now - fns));
        }
#endif
        av_frame_unref(vframe);

        /* IMP-7: block until the decode thread publishes a new frame OR the ~60Hz
         * budget elapses, then loop and present. This presents a fresh frame within
         * sub-ms (no waiting for the next poll tick) without busy-spinning; the
         * relative timeout == the old nanosleep budget (<=0 returns immediately),
         * so idle cadence (OSD, banner blink, events) is unchanged. `last_gen` is
         * the generation of the last frame taken, so a publish during this loop
         * body returns immediately (predicate already true). */
        int64_t spent = now_ns() - t;
        frame_slot_wait(&vslot, last_gen, render_interval_ns - spent);
    }

    dump_lan_stats(&vdec, "exit");   /* final tally: one run -> a drop verdict */

    av_frame_free(&vframe);
    av_frame_free(&shown);
    for (int i = 0; i < pqn; i++) av_frame_free(&pq[i]);
    render_destroy(r);
    if (browsing) mdns_browser_stop(&browser);
    if (cctl_started) control_client_stop(&cctl);   /* sends UNSUBSCRIBE */
    telem_recv_stop(&trecv);
    video_decode_stop(&vdec);
    frame_slot_destroy(&vslot);
    return 0;
}
