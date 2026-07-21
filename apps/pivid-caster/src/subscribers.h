/* apps/pivid-caster/src/subscribers.h -- the caster's fixed-size subscriber
 * table. Pure logic, no sockets: upsert (= renew/keepalive), remove, age-out,
 * and the loopback-accept gate. Keyed by (source IP, control source port):
 * each client instance owns one control socket, so the pair identifies the
 * INSTANCE -- two viewers on one host (e.g. the RX-local player and a
 * udp_tap capture, both 127.0.0.1) must not clobber each other's slot (each
 * keepalive re-pointed video_port, starving the other in a sawtooth; RCA
 * 2026-07-05). No wire change: the port comes from recvfrom's src addr.
 * `now_ms` is passed in so aging is testable without a clock. */
#ifndef CASTER_SUBSCRIBERS_H
#define CASTER_SUBSCRIBERS_H
#include <stdbool.h>
#include <stdint.h>

#define CASTER_MAX_SUBSCRIBERS  8
#define CASTER_SUB_TTL_MS       5000   /* drop entries not renewed within 5 s */

typedef struct {
    uint32_t ip;            /* source address, network byte order (sin_addr.s_addr) */
    uint16_t ctrl_port;     /* control-socket source port (network order); with ip,
                             * identifies the client INSTANCE */
    uint16_t video_port;    /* HOST order; where to unicast video    */
    uint16_t telem_port;    /* HOST order; where to unicast telemetry */
    int64_t  last_seen_ms;
    bool     used;
} Subscriber;

typedef struct {
    Subscriber e[CASTER_MAX_SUBSCRIBERS];
} SubTable;

/* Zero the table. */
void subs_init(SubTable *t);

/* Insert-or-renew by (`ip`, `ctrl_port`): if the instance is already present,
 * update its ports + last_seen (the keepalive path); else claim a free slot.
 * Returns 0 on success, -1 if the table is full and the instance is new
 * (caller logs the rejection; never a silent drop). */
int subs_upsert(SubTable *t, uint32_t ip, uint16_t ctrl_port,
                uint16_t video_port, uint16_t telem_port, int64_t now_ms);

/* Remove the entry for (`ip`, `ctrl_port`) if present (clean UNSUBSCRIBE).
 * No-op if absent. */
void subs_remove(SubTable *t, uint32_t ip, uint16_t ctrl_port);

/* Drop every entry older than `ttl_ms` relative to `now_ms`. Returns the count
 * removed. */
int subs_age_out(SubTable *t, int64_t now_ms, int64_t ttl_ms);

/* Number of live entries. */
int subs_count(const SubTable *t);

/* The control-socket accept gate: a datagram from `src_ip` (network order) is
 * processed only if forwarding is enabled OR the source is loopback
 * (127.0.0.1). When forwarding is OFF, a non-loopback datagram is dropped
 * BEFORE parsing -- the remote-access boundary. */
bool subs_src_allowed(uint32_t src_ip, bool forward_enabled);

#endif
