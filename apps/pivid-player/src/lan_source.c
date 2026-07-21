/* apps/pivid-player/src/lan_source.c -- see lan_source.h. */
#define _POSIX_C_SOURCE 200809L
#include "lan_source.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LAN_SOURCE_RECV_BUF (1 << 20)

/* --- POSIX socket helpers (the ONLY raw-socket surface; a future Windows
 *     net_compat/Winsock shim swaps just these). ------------------------- */
static int  ls_socket_udp(void) { return socket(AF_INET, SOCK_DGRAM, 0); }
static void ls_close(int fd) { if (fd >= 0) close(fd); }
static void ls_set_rcvbuf(int fd, int b) { setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&b,sizeof b); }
static void ls_set_rcvtimeo_ms(int fd, int ms) {
    struct timeval tv = { ms/1000, (ms%1000)*1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
static uint64_t mono_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

/* --- AU ring ------------------------------------------------------------- */
static void ring_push(LanSource *s, const uint8_t *au, size_t len) {
    if (len == 0 || len > AU_ASM_CAP) return;
    if (s->au_count == LAN_SOURCE_AU_SLOTS) {          /* full: drop oldest */
        s->au_head = (s->au_head + 1) % LAN_SOURCE_AU_SLOTS;
        s->au_count--; s->au_overrun++;
    }
    memcpy(s->au[s->au_tail].buf, au, len);
    s->au[s->au_tail].len = len;
#ifdef LATPROBE
    s->au[s->au_tail].ingress_ns = s->last_recv_ns;
#endif
    s->au_tail = (s->au_tail + 1) % LAN_SOURCE_AU_SLOTS;
    s->au_count++;
}
int lan_source_pop_au(LanSource *s, const uint8_t **au, size_t *len) {
    if (s->au_count == 0) return 0;
    *au  = s->au[s->au_head].buf;
    *len = s->au[s->au_head].len;
#ifdef LATPROBE
    s->au_ingress_ns = s->au[s->au_head].ingress_ns;   /* before au_head advances */
#endif
    s->au_head = (s->au_head + 1) % LAN_SOURCE_AU_SLOTS;
    s->au_count--;
    return 1;
}

#ifdef LATPROBE
uint64_t lan_source_cur_ingress_ns(const LanSource *s) { return s->au_ingress_ns; }
#endif

/* callbacks: lan_reasm -> au_asm -> ring */
static void on_nal(const uint8_t *nal, size_t len, void *user) {
    au_asm_feed(&((LanSource *)user)->auasm, nal, len);
}
static void on_au(const uint8_t *au, size_t len, void *user) {
    ring_push((LanSource *)user, au, len);
}

void lan_source_reset(LanSource *s) {
    s->au_head = s->au_tail = s->au_count = 0;
    s->last_data_ns = 0;
    s->idle_flush_ns = (uint64_t)LAN_SOURCE_IDLE_FLUSH_MS_DEFAULT * 1000000ull;
    lan_reasm_init(&s->reasm, on_nal, s);
    au_asm_init(&s->auasm, on_au, s);
}
void lan_source_feed_datagram(LanSource *s, const uint8_t *dg, size_t dg_len, uint64_t now_ns) {
#ifdef LATPROBE
    s->last_recv_ns = now_ns;   /* AU-completing callbacks below stamp with this */
#endif
    lan_reasm_feed(&s->reasm, dg, dg_len, now_ns);
    lan_reasm_tick(&s->reasm, now_ns);
    s->last_data_ns = now_ns;
}
void lan_source_on_timeout(LanSource *s, uint64_t now_ns) {
    lan_reasm_tick(&s->reasm, now_ns);   /* independent 150ms reasm stale flush */
    if (s->idle_flush_ns && now_ns - s->last_data_ns >= s->idle_flush_ns)
        au_asm_flush(&s->auasm);         /* wire idle past threshold: present AU early */
}

int lan_source_open(LanSource *s, uint16_t port, atomic_bool *stop, uint16_t *out_port) {
    memset(s, 0, sizeof *s);
    s->fd = -1; s->stop = stop;
    lan_source_reset(s);
    const char *env = getenv("PIVID_PLAYER_IDLE_FLUSH_MS");
    if (env && *env) {
        long ms = strtol(env, NULL, 10);
        if (ms < 0) ms = 0;
        if (ms > LAN_SOURCE_IDLE_FLUSH_MS_MAX) ms = LAN_SOURCE_IDLE_FLUSH_MS_MAX;
        s->idle_flush_ns = (uint64_t)ms * 1000000ull;   /* 0 => disable idle-flush */
    }
    int fd = ls_socket_udp();
    if (fd < 0) return -1;
    struct sockaddr_in sa; memset(&sa,0,sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons(port);
    if (bind(fd,(struct sockaddr*)&sa,sizeof sa) < 0) { ls_close(fd); return -1; }
    ls_set_rcvbuf(fd, LAN_SOURCE_RECV_BUF);
    ls_set_rcvtimeo_ms(fd, LAN_SOURCE_RCVTIMEO_MS);
    struct sockaddr_in b; socklen_t bl = sizeof b;
    if (getsockname(fd,(struct sockaddr*)&b,&bl)!=0) { ls_close(fd); return -1; }
    if (out_port) *out_port = ntohs(b.sin_port);
    s->fd = fd;
    return 0;
}

int lan_source_next_au(LanSource *s, const uint8_t **au, size_t *len) {
    for (;;) {
        if (lan_source_pop_au(s, au, len)) return LAN_SOURCE_OK;
        if (atomic_load(s->stop)) return LAN_SOURCE_STOP;
        uint8_t dg[LANFRAG_HDR_BYTES + LANFRAG_CHUNK_PAYLOAD + 64];
        ssize_t r = recv(s->fd, dg, sizeof dg, 0);
        uint64_t now = mono_ns();
        if (r > 0) {
            lan_source_feed_datagram(s, dg, (size_t)r, now);
        } else if (r == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            lan_source_on_timeout(s, now);   /* recv wake: tick + threshold-gated flush */
        } else {
            fprintf(stderr, "lan_source: recv error: %s -- stopping decode\n",
                    strerror(errno));
            return LAN_SOURCE_STOP;    /* fatal socket error: exit cleanly, no spin */
        }
    }
}

void lan_source_close(LanSource *s) { ls_close(s->fd); s->fd = -1; }
