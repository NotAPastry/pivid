/* H.264 camera pipe -> chunked + framed packets -> kernel TX ring.
 *
 * One camera pipe read() is one "fragmented unit": the first chunk gets START,
 * the last chunk gets END, intermediates get neither. SEQ is a global
 * 4-bit counter that increments per packet across all read boundaries.
 * The encoder is stateless beyond seq_counter + last_data_ns. */

#define _POSIX_C_SOURCE 200809L

#include "encoder.h"
#include "protocol.h"
#include "pkt_hist.h"
#include "resid_hist.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define READ_BLOCK_BYTES  4096
#define STATS_PERIOD_MS   5000ull       /* heartbeat cadence */
#define POLL_TIMEOUT_MS   200           /* short enough to drive heartbeat cleanly */

/* Probe instrumentation: produced packet-size histogram, dumped on SIGUSR1. */
static PktHist g_pkt_hist;
static volatile sig_atomic_t g_pkt_hist_dump_req;

/* Probe instrumentation: NAL parser residency histogram (sizes IMP-1, the
 * tail-of-frame hold), dumped alongside g_pkt_hist on SIGUSR1. */
static ResidHist g_s1_parser;
static const uint64_t S1_EDGES[] = {
    500000, 1000000, 2000000, 4000000, 8000000, 16000000,
    24000000, 32000000, 40000000, 50000000, 70000000, (uint64_t)-1 };

static void pkt_hist_sigusr1(int sig) {
    (void)sig;
    g_pkt_hist_dump_req = 1;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Push shim: FecTx -> tx_push. user is the Encoder. */
static void fec_push_to_ring(const uint8_t *pkt, size_t len, void *user) {
    Encoder *e = (Encoder *)user;
    /* Probe: record EVERY produced packet's size before any debug drop, so the
     * histogram reflects the true produced-size distribution. */
    pkt_hist_add(&g_pkt_hist, len);
    /* DEBUG: deterministic pre-transmit loss injection (PIVID_TX_DROP_PCT).
     * Clean on-air-loss emulation -- RX timing is unaffected, so this does NOT
     * amplify the kernel SPI misframe the way an RX-side drop does. */
    if (e->tx_drop_pct) {
        e->tx_drop_state += e->tx_drop_pct;
        if (e->tx_drop_state >= 100) { e->tx_drop_state -= 100; e->tx_injected_drops++; return; }
    }
    /* A full ring is a microburst (IDR FEC flush): the kernel drains a slot
     * every ~2.5 ms, so wait briefly and retry rather than silently losing
     * the packet. Unprotected passthrough NALs (droppable tail slices) have
     * no parity -- a silent drop here IS a visible slice loss at the player
     * (bottom-half corruption RCA, 2026-07-05). -2 (doorbell fail) means the
     * packet IS enqueued; only -1 (full) retries. Bounded: ~50 ms worst case,
     * backpressure into the 1 MB camera pipe, then one final counted push. */
    for (int try = 0; try < TX_PUSH_TRIES; try++) {
        if (tx_push_try(e->dev, pkt, len) != -1) {
            e->tx_push_retries += (uint64_t)try;
            return;
        }
        struct timespec ts = { 0, TX_PUSH_RETRY_WAIT_US * 1000L };
        nanosleep(&ts, NULL);
    }
    e->tx_push_retries += TX_PUSH_TRIES;
    (void)tx_push(e->dev, pkt, len);   /* final attempt; tx_push counts the drop */
}

void encoder_init(Encoder *e, TX_Dev *dev, int cam_fd, int wake_fd,
                  const EncoderConfig *cfg) {
    memset(e, 0, sizeof(*e));
    encoder_nal_parser_init(&e->parser);
    e->dev      = dev;
    e->cam_fd   = cam_fd;
    e->wake_fd  = wake_fd;
    e->last_stats_ms = mono_ms();
    /* seq_counter, last_data_ns start at 0 via memset. */

    /* Probe instrumentation: init histograms + SIGUSR1 dump handler. */
    pkt_hist_init(&g_pkt_hist);
    resid_hist_init(&g_s1_parser, "s1_parser", S1_EDGES,
                    (int)(sizeof S1_EDGES / sizeof S1_EDGES[0]), 5000000ull);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = pkt_hist_sigusr1;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    /* FEC parameters come from EncoderConfig (filled by main from Settings).
     * fec_tx_init clamps to [0,FEC_M_MAX] / [1,FEC_K_MAX].
     * Outer framing is PROTO_MODE_FIXED ON PURPOSE, even though the chip runs
     * DYNAMIC: the 2-byte LE16 length field is load-bearing for the RX driver's
     * per-packet SPI frame-sync (it disambiguates the leading stray byte via an
     * exact pkt_len-3 match -- DYNAMIC video has no such field and misframes,
     * proven 57-88% ambiguous offline). DYNAMIC was evaluated and rejected; the
     * chip is pinned DYNAMIC so the two cannot drift. See
     * docs/protocol/02-outer-packet.md and docs/DECISIONS.md. */
    fec_tx_init(&e->fec, cfg->fec_m_anchor, cfg->fec_m_ref, cfg->fec_k,
                &e->seq_counter, PROTO_MODE_FIXED, fec_push_to_ring, e);

    /* DEBUG ONLY: clean on-air loss injection. Off (0) in production. */
    e->tx_drop_pct = 0;
    const char *td = getenv("PIVID_TX_DROP_PCT");
    if (td) { int v = atoi(td); if (v > 0 && v <= 100) e->tx_drop_pct = (uint32_t)v; }

    /* Periodic SPS/PPS re-injection: needed for the intra-refresh (GDR) recipe
     * where i_period=0 means params are sent only once at startup. 0 = off. */
    uint64_t reinject_ns = (cfg->sps_reinject_ms > 0)
                         ? (uint64_t)cfg->sps_reinject_ms * 1000000ULL : 0;
    sps_reinject_init(&e->sps_reinject, reinject_ns);

    /* Feeder-level AQM. Mode + recipe come from cfg->aqm (filled by main from
     * Settings); tuning knobs stay hidden env. OFF (default) leaves it dormant
     * and aqm_filter_nal returns KEEP on a single branch. dev->tx_ring is
     * already mapped (tx_open precedes encoder_init in main). */
    (void)aqm_start(&e->aqm, dev, &cfg->aqm);
}

void encoder_set_input_fd(Encoder *e, int cam_fd) {
    e->cam_fd = cam_fd;
}

/* Adapter: receive one parsed NAL from the parser, route it through the
 * FEC stage (which assigns SEQ, builds protocol packets, and pushes to
 * tx_ring via fec_push_to_ring). The void* user is the Encoder. */
/* Route a (re-injected) parameter-set NAL through the same FEC/chunk path. */
static void reinject_emit(const uint8_t *nal, size_t len, void *user) {
    Encoder *e = (Encoder *)user;
    fec_tx_nal(&e->fec, nal, len);
}

static void emit_nal_to_chunker(const uint8_t *nal, size_t len, void *user) {
    Encoder *e = (Encoder *)user;
    uint8_t trailer_pad[4];
    if (len == 0) return;            /* empty NAL from back-to-back start codes */
    /* Filler (type 12, the --frame-trailer NAL): it rides the air as the AU-end
     * marker the player's au_asm closes each frame on, letting
     * the player release a frame without waiting for the NEXT frame's start
     * code (~33 ms glass-to-glass reclaim). rpicam emits a 2-byte runt (NAL
     * header + rbsp stop); as a PROTO_MODE_FIXED outer packet that is
     * flags(1)+LE16(2)+2 = 5 bytes on air -- one BELOW the LR2021 FLRC 6-byte
     * minimum payload (DS 18.2.2). Pad the filler with 0xFF ff_bytes to 3 so the
     * outer packet is >= 6: still a well-formed filler NAL, decoder-ignored, and
     * the length stays byte-exact end-to-end. Padding HERE (not in the driver)
     * keeps the outer LE16 consistent with the on-air length so the RX in-band
     * framer never sees a residue byte; the driver enforces the same floor
     * defensively (lr2021_tx_submit_locked). NOTE: transmitting this runt was
     * unsafe until the RX driver's coalesced-RX_DONE misframing was fixed (the
     * walker in lr2021_rx_walk.h, RCA 2026-07-05) -- it landed inside the SPI
     * drain of the preceding tail-slice packet and truncated it. */
    if ((nal[0] & 0x1F) == 12 && len < 3) {
        size_t pad = 3 - len;
        trailer_pad[0] = nal[0];
        memset(trailer_pad + 1, 0xFF, pad);
        memcpy(trailer_pad + 1 + pad, nal + 1, len - 1);
        nal = trailer_pad;
        len = 3;
    }
    /* IMP-1 probe: how long did this NAL sit in the parser before we could emit
     * it? emit_arrival_ns is the arrival of its last content byte; feed_now_ns
     * is "now" (the arrival of the bytes that carried the next start code and
     * triggered this emit). Skip when unstamped (feed_now_ns==0: tests/EOF). */
    if (e->parser.feed_now_ns && e->parser.emit_arrival_ns &&
        e->parser.feed_now_ns >= e->parser.emit_arrival_ns) {
        resid_hist_add(&g_s1_parser,
                       e->parser.feed_now_ns - e->parser.emit_arrival_ns);
    }
    /* Cache SPS/PPS and, on a coded slice past the interval, re-emit them first
     * so a GDR stream stays re-acquirable on the one-way link. No-op when the
     * re-inject interval is 0 (default). */
    sps_reinject_on_nal(&e->sps_reinject, nal, len, mono_ns(), reinject_emit, e);
    /* AQM frame-drop gate: returns 0 to suppress a congestion-dropped AU's VCL
     * NALs before they reach the FEC packer. KEEP (1) when the gate is OFF. */
    if (!aqm_filter_nal(&e->aqm, nal, len)) return;
    fec_tx_nal(&e->fec, nal, len);
}

int encoder_run(Encoder *e) {
    uint8_t buf[READ_BLOCK_BYTES];

    for (;;) {
        if (g_pkt_hist_dump_req) {
            g_pkt_hist_dump_req = 0;
            pkt_hist_dump(&g_pkt_hist, STDERR_FILENO);
            resid_hist_dump(&g_s1_parser, STDERR_FILENO);
#ifdef LATPROBE
            fec_tx_probe_dump(STDERR_FILENO);
#endif
        }

        struct pollfd pfds[2];
        pfds[0].fd = e->cam_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = e->wake_fd;  pfds[1].events = POLLIN; pfds[1].revents = 0;

        int pr = poll(pfds, 2, POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        /* Wake first: a wake during a populated camera pipe shouldn't drop data,
         * but the caller's intent (bind / shutdown) takes priority over
         * draining the next 4 KB. Main calls encoder_run again to resume. */
        if (pfds[1].revents & POLLIN) {
            uint64_t drain;
            (void)read(e->wake_fd, &drain, sizeof drain);  /* eventfd drain */
            return 2;
        }

        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(e->cam_fd, buf, sizeof buf);
            if (n == 0) {
                encoder_nal_parser_finish(&e->parser, emit_nal_to_chunker, e);
                fec_tx_flush(&e->fec);
                return 1;       /* clean EOF */
            }
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            /* Stamp the arrival time once and reuse it: the residency probe
             * needs it as each NAL's content-arrival clock (parser.feed_now_ns),
             * and the GPIO TX LED reads last_data_ns (sub-us earlier than the
             * old post-feed store; harmless at its <1s staleness tolerance). */
            uint64_t now = mono_ns();
            e->parser.feed_now_ns = now;
            encoder_nal_parser_feed(&e->parser, buf, (size_t)n,
                                    emit_nal_to_chunker, e);
            __atomic_store_n(&e->last_data_ns, now, __ATOMIC_RELAXED);
        } else if (pfds[0].revents & (POLLHUP | POLLERR)) {
            /* camera pipe closed without POLLIN (POLLHUP-only) -- treat as EOF. */
            encoder_nal_parser_finish(&e->parser, emit_nal_to_chunker, e);
            fec_tx_flush(&e->fec);
            return 1;
        }

        /* Heartbeat tick: surface to main so it can log stats. Main is
         * responsible for advancing last_stats_ms before re-entering. */
        if (mono_ms() - e->last_stats_ms >= STATS_PERIOD_MS) {
            return 3;
        }
    }
}

void encoder_set_fec(Encoder *e, int m_anchor, int m_ref, int k) {
    if (m_anchor < 0 || m_anchor > FEC_M_MAX) return;
    if (m_ref < 0 || m_ref > FEC_M_MAX) return;
    if (k < 1 || k > FEC_K_MAX) return;
    fec_tx_flush(&e->fec);   /* close any open block on the old config */
    fec_tx_init(&e->fec, m_anchor, m_ref, k, &e->seq_counter,
                PROTO_MODE_FIXED, fec_push_to_ring, e);
}

int encoder_get_fec_anchor(const Encoder *e) { return e->fec.m_anchor; }
int encoder_get_fec_k(const Encoder *e)      { return e->fec.k_bound; }

#define NP_STATE_IDLE     0
#define NP_STATE_MATCHING 1
#define NP_STATE_IN_NAL   2

static void parser_emit_and_reset(Encoder_NalParser *p,
                                  Encoder_NalEmit emit, void *user) {
    if (p->nal_len > 0) {
        /* Residency probe: publish this NAL's content-arrival clock so the emit
         * callback can compute how long it sat buffered (see g_s1_parser). */
        p->emit_arrival_ns = p->cur_arrival_ns;
        emit(p->nal_buf, p->nal_len, user);
        p->nals_emitted++;
    }
    p->nal_len = 0;
    p->state   = NP_STATE_IDLE;
    p->zeros   = 0;
    p->cur_arrival_ns = 0;   /* next NAL starts fresh */
}

void encoder_nal_parser_init(Encoder_NalParser *p) {
    memset(p, 0, sizeof *p);
    p->state = NP_STATE_IDLE;
}

void encoder_nal_parser_feed(Encoder_NalParser *p,
                             const uint8_t *buf, size_t n,
                             Encoder_NalEmit emit, void *user) {
    /* Annex-B start-code scanner.
     *
     * Start code = 3 or 4 bytes: optional 0x00 + 0x00 0x00 0x01.
     * We don't disambiguate 3-byte vs 4-byte; trailing 0x00 bytes
     * before the 0x00 0x00 0x01 are treated as belonging to the start
     * code (they get rolled back from the in-progress NAL buffer).
     *
     * State machine:
     *   IDLE / IN_NAL  - copy byte into nal_buf. If we see 0x00,
     *                    tentatively transition to MATCHING but keep
     *                    the byte in the buffer.
     *   MATCHING       - if next byte is 0x00, stay in MATCHING and
     *                    increment p->zeros (still buffer the byte).
     *                    If next is 0x01 with zeros >= 2, we've found
     *                    a start code: roll back the (zeros) 0x00s we
     *                    speculatively wrote, emit the prior NAL, and
     *                    enter IN_NAL waiting for the first real byte.
     *                    Anything else: not a start code; return to
     *                    IDLE/IN_NAL, leave bytes in place. */
    for (size_t i = 0; i < n; i++) {
        uint8_t b = buf[i];

        switch (p->state) {
        case NP_STATE_IDLE:
        case NP_STATE_IN_NAL:
            if (b == 0x00) {
                if (p->nal_len < ENCODER_NAL_BUF_BYTES)
                    p->nal_buf[p->nal_len++] = b;
                p->zeros = 1;
                p->state = NP_STATE_MATCHING;
                /* speculative: may be the next start code -> don't stamp arrival */
            } else {
                if (p->nal_len < ENCODER_NAL_BUF_BYTES)
                    p->nal_buf[p->nal_len++] = b;
                else
                    p->nals_oversize++;  /* truncated */
                p->cur_arrival_ns = p->feed_now_ns;  /* definite NAL content */
            }
            break;

        case NP_STATE_MATCHING:
            if (b == 0x00) {
                if (p->nal_len < ENCODER_NAL_BUF_BYTES)
                    p->nal_buf[p->nal_len++] = b;
                p->zeros++;
            } else if (b == 0x01 && p->zeros >= 2) {
                /* Start code confirmed. Roll back the (zeros) 0x00s
                 * that we speculatively added; they belong to the
                 * start code, not the prior NAL. */
                if (p->nal_len >= p->zeros)
                    p->nal_len -= p->zeros;
                parser_emit_and_reset(p, emit, user);
                p->state = NP_STATE_IN_NAL;
                p->zeros = 0;
                /* This 0x01 is part of the start code; don't buffer it. */
            } else {
                /* False alarm. The bytes we buffered are real NAL bytes. */
                if (p->nal_len < ENCODER_NAL_BUF_BYTES)
                    p->nal_buf[p->nal_len++] = b;
                else
                    p->nals_oversize++;
                p->cur_arrival_ns = p->feed_now_ns;  /* the buffered 0x00s + b are content */
                p->state = (p->nal_len > 0) ? NP_STATE_IN_NAL : NP_STATE_IDLE;
                p->zeros = 0;
            }
            break;
        }
    }
}

void encoder_nal_parser_finish(Encoder_NalParser *p,
                               Encoder_NalEmit emit, void *user) {
    parser_emit_and_reset(p, emit, user);
}
