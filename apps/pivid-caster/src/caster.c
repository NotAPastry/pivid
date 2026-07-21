/* apps/pivid-caster/src/caster.c -- see caster.h. No avahi here. */
#define _GNU_SOURCE          /* SO_RCVBUFFORCE (root bypass of rmem_max) */
#include "caster.h"
#include "control.h"
#include "loopback.h"
#include "lanfrag.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>   /* struct iovec, sendmsg */
#include <unistd.h>

#ifdef LATPROBE
#include "resid_hist.h"
#include "mono_ns.h"
static ResidHist g_s6b_caster_fwd;
static const uint64_t S6B_EDGES[] = {
    100000, 250000, 500000, 1000000, 2000000, 4000000,
    8000000, 16000000, 24000000, 33000000, (uint64_t)-1 };
void caster_probe_dump(int fd) { resid_hist_dump(&g_s6b_caster_fwd, fd); }
#endif

/* Max UDP payload we will forward in one datagram. A large IDR NAL from rx can
 * approach the IP datagram ceiling; 65535 covers the worst case. */
#define CASTER_DGRAM_MAX 65535

static int open_bound(uint32_t bind_addr_net, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("caster socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = bind_addr_net;
    sa.sin_port = htons(port);
    if (bind(fd, (const struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("caster bind"); close(fd); return -1;
    }
    return fd;
}

/* Enlarge a socket's receive buffer past the default. SO_RCVBUFFORCE bypasses
 * net.core.rmem_max but needs CAP_NET_ADMIN -- the caster runs as root on the
 * Pi, so it takes effect there; if it fails (unprivileged, e.g. the test
 * container) fall back to plain SO_RCVBUF, which still bumps above the default
 * (clamped to rmem_max). */
static void set_rcvbuf(int fd, int bytes) {
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bytes, sizeof bytes) == 0)
        return;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
}

int caster_open(CasterCtx *c, bool forward_enabled) {
    memset(c, 0, sizeof *c);
    c->video_fd = c->telem_fd = c->ctrl_fd = c->fwd_fd = -1;
    c->forward_enabled = forward_enabled;
    subs_init(&c->subs);
#ifdef LATPROBE
    resid_hist_init(&g_s6b_caster_fwd, "s6b_caster_fwd", S6B_EDGES,
                    (int)(sizeof S6B_EDGES / sizeof S6B_EDGES[0]), 2000000ull);
#endif

    uint32_t loop = htonl(INADDR_LOOPBACK);
    c->video_fd = open_bound(loop, (uint16_t)LOOPBACK_VIDEO_PORT);
    c->telem_fd = open_bound(loop, (uint16_t)LOOPBACK_TELEM_PORT);
    c->ctrl_fd  = open_bound(htonl(INADDR_ANY), (uint16_t)CASTER_CTRL_PORT);

    /* Enlarge the ingest socket buffers so an rx->caster burst (an 8-NAL IDR
     * arriving back-to-back, up to ~8 x 64 KB) is queued, not dropped at the
     * socket before the poll loop drains it. ctrl_fd is left at the default --
     * control datagrams are tiny and infrequent. */
    if (c->video_fd >= 0) set_rcvbuf(c->video_fd, 1 << 20);
    if (c->telem_fd >= 0) set_rcvbuf(c->telem_fd, 1 << 20);

    /* Dedicated egress socket for fan-out. It MUST be separate from the
     * loopback-bound ingest sockets and itself UNBOUND. A socket bound to
     * 127.0.0.1 pins its source address to loopback, so a sendto() to a LAN
     * subscriber (e.g. 10.0.0.5) would emit a martian src=127.0.0.1 packet the
     * kernel rejects with EINVAL -- and silently, since fan_out only counts
     * full sends. Leaving fwd_fd unbound lets the kernel choose the correct
     * egress-interface source per destination: loopback for the local HDMI
     * player, the real LAN IP for remote PCs. A generous SO_SNDBUF absorbs an
     * 8-subscriber IDR fan-out burst (~8 x 65 KB) without ENOBUFS. */
    c->fwd_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (c->fwd_fd >= 0) {
        int sndbuf = 1 << 20;   /* ~1 MB */
        setsockopt(c->fwd_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
        /* Non-blocking egress: if the 1 MB SO_SNDBUF still fills during an
         * 8-way IDR fan-out, sendmsg/sendto returns EAGAIN/ENOBUFS (counted as
         * a send-fail) rather than blocking the single-threaded poll loop --
         * which would stall video/telem/control ingest behind one slow peer. */
    } else {
        perror("caster fwd socket");
    }

    if (c->video_fd < 0 || c->telem_fd < 0 || c->ctrl_fd < 0 || c->fwd_fd < 0) {
        caster_close(c);
        return -1;
    }
    return 0;
}

void caster_set_forward(CasterCtx *c, bool enabled) { c->forward_enabled = enabled; }

void caster_close(CasterCtx *c) {
    if (c->video_fd >= 0) close(c->video_fd);
    if (c->telem_fd >= 0) close(c->telem_fd);
    if (c->ctrl_fd  >= 0) close(c->ctrl_fd);
    if (c->fwd_fd   >= 0) close(c->fwd_fd);
    c->video_fd = c->telem_fd = c->ctrl_fd = c->fwd_fd = -1;
}

/* Telemetry / verbatim fan-out: one sendto per subscriber (unchanged behavior;
 * telem datagrams are small and never fragment). */
static void fan_out_verbatim(CasterCtx *c, const uint8_t *buf, size_t len) {
    for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++) {
        if (!c->subs.e[i].used) continue;
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof dst);
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = c->subs.e[i].ip;
        dst.sin_port = htons(c->subs.e[i].telem_port);
        ssize_t w = sendto(c->fwd_fd, buf, len, MSG_NOSIGNAL,
                           (const struct sockaddr *)&dst, sizeof dst);
        if (w == (ssize_t)len) { c->bytes_fwd += len; c->telem_fwd++; }
        else                   { c->telem_send_fail++; }
    }
}

/* Per-chunk emit context: send one chunk (8-byte header + payload slice) to
 * every live subscriber's video_port via sendmsg/iovec (zero payload copy). */
typedef struct { CasterCtx *c; } VidChunkCtx;

static void vid_chunk_emit(const uint8_t *hdr, size_t hdr_len,
                           const uint8_t *payload, size_t payload_len,
                           void *user) {
    CasterCtx *c = ((VidChunkCtx *)user)->c;
    struct iovec iov[2] = {
        { .iov_base = (void *)hdr,     .iov_len = hdr_len },
        { .iov_base = (void *)payload, .iov_len = payload_len },
    };
    size_t total = hdr_len + payload_len;
    for (int i = 0; i < CASTER_MAX_SUBSCRIBERS; i++) {
        if (!c->subs.e[i].used) continue;
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof dst);
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = c->subs.e[i].ip;
        dst.sin_port = htons(c->subs.e[i].video_port);
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        msg.msg_name    = &dst;
        msg.msg_namelen = sizeof dst;
        msg.msg_iov     = iov;
        msg.msg_iovlen  = 2;
        ssize_t w = sendmsg(c->fwd_fd, &msg, MSG_NOSIGNAL);
        if (w == (ssize_t)total) { c->bytes_fwd += total; c->video_fwd++; }
        else                     { c->chunk_send_fail++; }
    }
}

/* Video fan-out: chunk the framed NAL so no datagram IP-fragments. */
static void fan_out_video(CasterCtx *c, const uint8_t *nal, size_t len) {
    VidChunkCtx ctx = { .c = c };
    size_t chunks = lanfrag_chunk_nal(c->vid_nal_id, nal, len,
                                      vid_chunk_emit, &ctx);
    if (chunks == 0) { c->oversize_drops++; return; }  /* len==0 or > MAX */
    c->vid_nal_id++;   /* next NAL gets the next id (wraps at 65536) */
}

static void handle_video(CasterCtx *c) {
    uint8_t buf[CASTER_DGRAM_MAX];
    /* MSG_TRUNC: recv returns the true datagram size even if it exceeds buf, so
     * a rare > 64 KB framed NAL is dropped+counted instead of silently
     * truncated and forwarded corrupt. */
    ssize_t n = recv(c->video_fd, buf, sizeof buf, MSG_TRUNC);
    if (n <= 0) return;
    c->video_in++;
    if ((size_t)n > LANFRAG_MAX_NAL) { c->oversize_drops++; return; }
#ifdef LATPROBE
    uint64_t t0 = pv_mono_ns();
#endif
    fan_out_video(c, buf, (size_t)n);
#ifdef LATPROBE
    { uint64_t now = pv_mono_ns(); if (now >= t0) resid_hist_add(&g_s6b_caster_fwd, now - t0); }
#endif
}

static void handle_telem(CasterCtx *c) {
    uint8_t buf[CASTER_DGRAM_MAX];
    ssize_t n = recv(c->telem_fd, buf, sizeof buf, 0);
    if (n <= 0) return;
    c->telem_in++;
    fan_out_verbatim(c, buf, (size_t)n);
}

static void handle_ctrl(CasterCtx *c, int64_t now_ms) {
    uint8_t buf[256];
    struct sockaddr_in src;
    socklen_t sl = sizeof src;
    ssize_t n = recvfrom(c->ctrl_fd, buf, sizeof buf, 0,
                         (struct sockaddr *)&src, &sl);
    if (n <= 0) return;

    /* Remote-access boundary: when forwarding is OFF, a non-loopback datagram is
     * dropped BEFORE parsing. */
    if (!subs_src_allowed(src.sin_addr.s_addr, c->forward_enabled)) {
        c->ctrl_dropped++;
        return;
    }
    CasterCtrlMsg m;
    if (caster_ctrl_parse(buf, (size_t)n, &m) != 0) { c->ctrl_dropped++; return; }

    if (m.type == CASTER_MSG_SUBSCRIBE) {
        if (subs_upsert(&c->subs, src.sin_addr.s_addr, src.sin_port,
                        m.video_port, m.telem_port, now_ms) < 0) {
            c->subs_rejected++;
            fprintf(stderr, "pivid-caster: subscriber table full (%d), "
                            "rejecting %s\n",
                    CASTER_MAX_SUBSCRIBERS, inet_ntoa(src.sin_addr));
        }
    } else if (m.type == CASTER_MSG_UNSUBSCRIBE) {
        subs_remove(&c->subs, src.sin_addr.s_addr, src.sin_port);
    }
}

int caster_step(CasterCtx *c, int timeout_ms, int64_t now_ms) {
    struct pollfd p[3] = {
        { .fd = c->ctrl_fd,  .events = POLLIN },
        { .fd = c->video_fd, .events = POLLIN },
        { .fd = c->telem_fd, .events = POLLIN },
    };
    int rc = poll(p, 3, timeout_ms);
    if (rc < 0) return (errno == EINTR) ? 0 : -1;

    int serviced = 0;
    if (p[0].revents & POLLIN) { handle_ctrl(c, now_ms);  serviced++; }
    if (p[1].revents & POLLIN) { handle_video(c);         serviced++; }
    if (p[2].revents & POLLIN) { handle_telem(c);         serviced++; }

    subs_age_out(&c->subs, now_ms, CASTER_SUB_TTL_MS);
    return serviced;
}
