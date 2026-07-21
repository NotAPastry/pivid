#define _DEFAULT_SOURCE          /* cfmakeraw(), CRTSCTS (glibc termios extensions) */
#define _POSIX_C_SOURCE 200809L
#include "msp_poll.h"
#include "msp_parse.h"
#include "msp_request.h"
#include "dp_client.h"
#include "osd_grid.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum { FC_OK = 0, FC_INIT = 1, FC_TIMEOUT = 2, FC_UART_ERROR = 3 };

#define FC_TIMEOUT_NS      500000000LL   /* >500 ms with no DP frame -> TIMEOUT */
#define UART_REOPEN_NS    1000000000LL   /* throttle open() syscalls to ~1 Hz */
#define LOG_RATELIMIT_NS  5000000000LL   /* suppress duplicate logs within a 5 s window */

static int64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Rate-limited log guard: returns true (and advances *last) on the first event
 * and at most once per 5 s window thereafter. */
static bool log_due(int64_t *last, int64_t now) {
    if (now - *last >= LOG_RATELIMIT_NS) { *last = now; return true; }
    return false;
}

static int open_uart(const char *dev, int baud) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { close(fd); return -1; }
    cfmakeraw(&tio);
    speed_t s = (baud == 57600) ? B57600 : B115200;
    cfsetispeed(&tio, s); cfsetospeed(&tio, s);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;          /* no flow control */
    tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { close(fd); return -1; }
    return fd;
}

static void *run(void *arg) {
    MSP_Poll *m = arg;
    MSP_Parser parser; msp_parser_init(&parser);
    DpClient dp; dp_client_init(&dp);
    uint8_t frame_id = 0;

    /* Configured keyframe TX cadence (read once; thread-private thereafter). */
    int kf_ms = m->settings->osd_keyframe_ms;
    if (kf_ms < 20 || kf_ms > 5000) kf_ms = 200;   /* fallback if unconfigured/out of range */
    const int64_t kf_ns = (int64_t)kf_ms * 1000000LL;

    int state = FC_INIT;
    atomic_store(&m->fc_link_state, state);

    /* Thread-private timers (only touched here in run(), so non-atomic).
     * Seed the "last X" stamps one interval in the PAST (not INT64_MIN): the
     * gates are `now - last >= INTERVAL`, and `now_ns() - INT64_MIN` overflows
     * int64 (UB, wraps negative) so the very first open()/log would never fire.
     * `now - INTERVAL` makes the first check true with no overflow. */
    int64_t last_open_attempt_ns = now_ns() - UART_REOPEN_NS;  /* due immediately */
    int64_t last_log_ns = now_ns() - LOG_RATELIMIT_NS;         /* first log allowed */
    int64_t last_frame_ns = now_ns();                          /* last DP frame received */
    int64_t next_kf_ns = now_ns() + kf_ns;

    while (!atomic_load(&m->stop)) {
        if (m->fd < 0 && now_ns() - last_open_attempt_ns >= UART_REOPEN_NS) {
            last_open_attempt_ns = now_ns();
            m->fd = open_uart(m->settings->msp_uart_device, m->settings->msp_baud);
            if (m->fd < 0) {
                /* Snapshot errno before now_ns() inside log_due() can perturb it,
                 * mirroring the fatal-read path. */
                int e = errno;
                if (log_due(&last_log_ns, now_ns()))
                    fprintf(stderr, "pivid-tx: MSP open %s failed (%s); retry 1s\n",
                            m->settings->msp_uart_device, strerror(e));
            } else {
                /* Per plan: nudge the FC into DisplayPort mode. Handshake
                 * direction is a HW-validation item (Phase 5); BF normally
                 * pushes DP frames once osd_displayport_device=MSP. */
                uint8_t hb[7];
                size_t hn = msp_request_build_payload(hb, MSP_DISPLAYPORT,
                                                      (uint8_t[]){MSP_DP_HEARTBEAT}, 1);
                (void)write(m->fd, hb, hn);  /* short write tolerated; FC repolls */
                last_frame_ns = now_ns();
                state = FC_INIT;
            }
        }

        ssize_t r = -1;
        if (m->fd >= 0) {
            uint8_t rbuf[256];
            r = read(m->fd, rbuf, sizeof rbuf);
            if (r > 0) {
                for (ssize_t i = 0; i < r; i++) {
                    if (msp_parser_feed(&parser, rbuf[i]) == 1 &&
                        parser.cmd == MSP_DISPLAYPORT) {
                        (void)dp_client_apply(&dp, parser.payload, parser.size);
                        last_frame_ns = now_ns();
                        atomic_fetch_add(&m->polls_ok, 1);  /* DP frames applied */
                    }
                }
            } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                /* Fatal read error (e.g. ENXIO/EIO: adapter unplugged). Snapshot
                 * errno before close()/now_ns() clobber it, drop the wedged fd so
                 * the 1 s reopen path can recover a replugged FC. */
                int e = errno;
                close(m->fd); m->fd = -1;
                if (log_due(&last_log_ns, now_ns()))
                    fprintf(stderr, "pivid-tx: MSP UART %s fatal error (%s); reopening\n",
                            m->settings->msp_uart_device, strerror(e));
                state = FC_UART_ERROR;
            }
            /* r==0 or benign EAGAIN/EWOULDBLOCK/EINTR: no data now -> timer/sleep. */
        }

        if (now_ns() >= next_kf_ns) {
            if (dp.dirty) {
                uint8_t kf[OSD_GRID_MAX_FRAGS * OSD_GRID_FRAG_CHUNK];
                /* Encode the COMMITTED snapshot (last DRAW_SCREEN), not the live
                 * grid, so a keyframe never captures a mid-frame update. */
                int kn = osd_grid_encode(&dp.committed, kf, sizeof kf);
                if (kn > 0) {
                    uint8_t frags[OSD_GRID_MAX_FRAGS][OSD_GRID_FRAG_MAX];
                    size_t flens[OSD_GRID_MAX_FRAGS];
                    /* Stamp video+parity packets only -- subtract this gauge's
                     * own OSD-grid fragments (the sole non-video tx_push user,
                     * already tallied in kf_frags_tx) so the RX vid-LQ
                     * denominator matches its video-only numerator. Without this
                     * the OSD frags sit in the denominator but never the
                     * numerator, flooring LQ ~1-2% below 100 on a clean link.
                     * Staleness-tolerant racy read of packets_sent, as before. */
                    uint32_t txv = (uint32_t)(m->dev->packets_sent -
                                              atomic_load(&m->kf_frags_tx));
                    int nf = osd_grid_fragment(kf, (size_t)kn, frame_id++, txv, frags, flens);
                    for (int i = 0; i < nf; i++) {
                        uint8_t pkt[3 + OSD_GRID_FRAG_MAX];
                        int pn = protocol_build(pkt, sizeof pkt, PROTO_MODE_DYNAMIC,
                                    pivid_pack_flags(true, true, 0, PKT_OSD_GRID),
                                    frags[i], flens[i]);
                        if (pn > 0) {
                            (void)tx_push(m->dev, pkt, (size_t)pn);
                            atomic_fetch_add(&m->kf_frags_tx, 1);
                        }
                    }
                    /* Read-and-clear the frame-ready flag only once a keyframe was
                     * actually produced. A (defensive) kn<=0 encode failure leaves
                     * dirty set so the next timer tick retries instead of silently
                     * dropping the frame until the next FC DRAW_SCREEN. */
                    dp.dirty = false;
                }
            }
            next_kf_ns += kf_ns;
            /* Cycle-overrun clamp: if a stall pushed the deadline into the past,
             * resync to now so we wait one cadence instead of bursting a backlog. */
            if (next_kf_ns < now_ns())
                next_kf_ns = now_ns() + kf_ns;
        }

        /* FC link state from DP-frame freshness. */
        if (m->fd < 0) {
            state = FC_UART_ERROR;
        } else if (now_ns() - last_frame_ns <= FC_TIMEOUT_NS) {
            state = FC_OK;
        } else {
            state = FC_TIMEOUT;
        }
        atomic_store(&m->fc_link_state, state);
        atomic_store(&m->parse_resets, parser.parse_resets);
        atomic_store(&m->crc_failures, parser.crc_failures);

        /* If we read bytes this iteration, loop immediately to keep draining;
         * otherwise sleep briefly to avoid a busy-spin. */
        if (r <= 0) {
            struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL);  /* 2 ms */
        }
    }

    if (m->fd >= 0) {
        /* Per plan: release DisplayPort before closing the UART. */
        uint8_t rel[7];
        size_t rn = msp_request_build_payload(rel, MSP_DISPLAYPORT,
                                              (uint8_t[]){MSP_DP_RELEASE}, 1);
        (void)write(m->fd, rel, rn);
        close(m->fd); m->fd = -1;
    }
    return NULL;
}

int msp_poll_start(MSP_Poll *m, TX_Dev *dev, const Settings *s) {
    memset(m, 0, sizeof *m);
    m->fd = -1; m->dev = dev; m->settings = s;
    atomic_store(&m->stop, false);
    atomic_store(&m->fc_link_state, FC_INIT);
    if (!s->msp_enabled) return 0;     /* not started; thread_started stays false */
    if (pthread_create(&m->thread, NULL, run, m) != 0) {
        perror("pthread_create msp_poll");
        return -1;
    }
    m->thread_started = true;
    return 0;
}

void msp_poll_stop(MSP_Poll *m) {
    if (!m->thread_started) return;
    atomic_store(&m->stop, true);
    pthread_join(m->thread, NULL);
    m->thread_started = false;
}
