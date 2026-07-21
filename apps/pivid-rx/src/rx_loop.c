/* RX main loop: poll /dev/radio0, drain the mmap'd RX ring, feed
 * reassembler. Mirror of encoder_run on the TX side.
 *
 * The reassembler's emit callback (wired up in main.c) is what actually
 * delivers NALs to the configured Output_Sink -- this loop just owns the
 * poll+drain mechanics. */

#define _POSIX_C_SOURCE 200809L

#include "rx_loop.h"

#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define POLL_TIMEOUT_MS  100
#define POP_BUF_BYTES    513        /* >= the 511 B FLRC payload ceiling (was 256,
                                     * stale from the pre-FLRC-511 ring). ring_user_pop
                                     * returns -1 ("torn") for any record whose payload
                                     * exceeds out_max, so 256 here drops every >256 B
                                     * packet -- and the FLRC-511 FEC stream is ~90% full
                                     * 505 B packets. See common/ring_user.h (out_max>=511). */
#define STATS_PERIOD_MS  5000ull    /* heartbeat cadence */

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int rx_loop_should_drop(uint64_t *state, uint32_t pct) {
    if (pct == 0) return 0;
    *state += pct;
    if (*state >= 100) { *state -= 100; return 1; }
    return 0;
}

void rx_loop_init(RX_Loop *l, RX_Dev *dev, Reasm *r, FecRx *fec,
                  Output_Sink *sink, int wake_fd) {
    memset(l, 0, sizeof(*l));
    l->dev     = dev;
    l->reasm   = r;
    l->fec     = fec;
    l->sink    = sink;
    l->wake_fd = wake_fd;
    l->last_stats_ms = monotonic_ms();

    /* DEBUG ONLY: deterministic post-CRC loss injection for FEC verification.
     * Off (0) in production; set via env PIVID_RX_DROP_PCT (1..100). */
    l->drop_pct = 0;
    const char *dp = getenv("PIVID_RX_DROP_PCT");
    if (dp) { int v = atoi(dp); if (v > 0 && v <= 100) l->drop_pct = (uint32_t)v; }
    l->drop_state = 0;
    l->injected_drops = 0;
}

int rx_loop_drain_once(RX_Loop *l) {
    uint8_t buf[POP_BUF_BYTES];
    int popped = 0;

    for (;;) {
        int n = rx_pop(l->dev, buf, sizeof buf);
        if (n == 0) break;                     /* ring drained */
        if (n < 0) {
            /* Should not happen in practice -- ring_user_pop only returns
             * -1 on corruption. rx_pop has already bumped dropped_torn. */
            fprintf(stderr, "pivid-rx: torn ring record, breaking drain\n");
            break;
        }
        l->records_popped++;
        popped++;
        /* Single monotonic read per popped record: stamps liveness AND feeds
         * FEC, so both observe the same instant.
         * CRC-valid record delivered by the kernel -> link is receiving. */
        uint64_t now_ns = monotonic_ns();
        __atomic_store_n(&l->last_valid_rx_ns, now_ns, __ATOMIC_RELAXED);
        if (rx_loop_should_drop(&l->drop_state, l->drop_pct)) {
            l->injected_drops++;
            continue;                          /* DEBUG: simulate RF packet loss */
        }

        /* TYPE dispatch BEFORE the FEC stage. The flags byte is buf[0] in every
         * framing mode (protocol_build always writes flags first). OSD-grid
         * fragments are TYPE=1, FEC=0 and are complete self-contained packets, so
         * they must NOT be fed to fec_rx (that would corrupt the FEC block stream)
         * and must NOT advance the reassembler's expected_seq (TX emits them with
         * SEQ=0). Guard n>=1 before indexing buf[0] -- a runt must not over-read. */
        if (n >= 1) {
            enum pivid_packet_type type;
            pivid_unpack_flags(buf[0], NULL, NULL, NULL, &type);
            if (type == PKT_OSD_GRID) {
                if (buf[0] & PROTO_FLAG_FEC) {
                    /* TYPE=1 + FEC=1 is invalid per the wire-protocol matrix
                     * (grid packets are never FEC-wrapped); drop. TX never emits
                     * this, so it only occurs on RF corruption. (Malformed
                     * fragments are counted inside rx_grid_on_packet; this case
                     * never reaches reassembly, so just drop it.) */
                    continue;
                }
                /* TX builds each grid fragment as a DYNAMIC outer packet
                 * ([flags][payload]); protocol_parse derives payload_len = n - 1
                 * and bounds-checks the record before any copy (rejects a runt /
                 * over-range length). Reassembly state + counters live inside
                 * RxGrid; the return value is ignored here. */
                uint8_t flags, payload[PROTOCOL_MAX_PAYLOAD]; size_t plen;
                if (protocol_parse(buf, (size_t)n, PROTO_MODE_DYNAMIC,
                                   &flags, payload, &plen) == 0) {
                    (void)rx_grid_on_packet(l->grid, payload, plen);
                }
                continue;                      /* do NOT advance the video path */
            }
            /* TYPE=0 video: bump the grid's rx-video counter before FEC->reasm. */
            rx_grid_on_video(l->grid);
        }

        fec_rx_feed(l->fec, buf, (size_t)n, now_ns);
    }
    return popped;
}

int rx_loop_run(RX_Loop *l) {
    for (;;) {
        struct pollfd pfds[2] = {
            { .fd = l->dev->fd,  .events = POLLIN, .revents = 0 },
            { .fd = l->wake_fd,  .events = POLLIN, .revents = 0 },
        };
        int pr = poll(pfds, 2, POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        l->poll_wakes++;

        /* Wake takes priority over draining: bind/shutdown is more urgent
         * than the next few records. Main re-enters rx_loop_run to resume. */
        if (pfds[1].revents & POLLIN) {
            uint64_t drain;
            (void)read(l->wake_fd, &drain, sizeof drain);
            return 2;
        }

        if (pfds[0].revents & (POLLERR | POLLHUP)) {
            fprintf(stderr, "pivid-rx: /dev/radio0 poll error/hangup\n");
            return 1;
        }

        if (pfds[0].revents & POLLIN) {
            (void)rx_loop_drain_once(l);
        }

        /* Tick the FEC stage and the reassembler every iteration so that
         * stalled open blocks and in-progress NALs time out correctly even
         * when no new packets arrive. */
        fec_rx_tick(l->fec, monotonic_ns());
        reasm_tick(l->reasm);

        /* Heartbeat tick: surface to main so it can refresh RSSI and log.
         * Main is responsible for advancing last_stats_ms before re-entering. */
        if (monotonic_ms() - l->last_stats_ms >= STATS_PERIOD_MS) {
            return 3;
        }
    }
}
