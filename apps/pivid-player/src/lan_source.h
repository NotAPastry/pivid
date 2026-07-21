/* apps/pivid-player/src/lan_source.h -- UDP video source that reassembles
 * chunked NALs (lan_reasm) into whole access units (au_asm) and serves one AU
 * per call. Replaces the old AVIOContext byte-stream: video_decode now feeds
 * AUs straight to avcodec_send_packet (no libavformat). Single-threaded on the
 * decode thread. Socket helpers stay POSIX behind the .c (Windows shim later). */
#ifndef PLAYER_LAN_SOURCE_H
#define PLAYER_LAN_SOURCE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include "lan_reasm.h"
#include "au_asm.h"

/* Ring depth >= the most AUs one next_au cycle can produce: a recv-timeout runs
 * lan_reasm_tick (up to LAN_REASM_SLOTS=4 stale-slot NAL flushes -> up to 4 AU
 * emits) AND au_asm_flush (1 more) = 5 worst case; 6 leaves headroom so the
 * drop-oldest au_overrun branch stays unreachable. */
#define LAN_SOURCE_AU_SLOTS     6
#define LAN_SOURCE_RCVTIMEO_MS  8            /* recv wake cadence (responsive stop-check) */

/* Idle-flush threshold: present a buffered AU early only after the wire has
 * been silent this long (decoupled from the recv-wake cadence above). TRUE-
 * STALL FALLBACK ONLY: it must sit ABOVE the RX FEC block-hold worst case
 * (~100 ms on still scenes -- a k=16 block fills across several small frames,
 * so a frame's tail slices legitimately arrive that late). At 50 ms it fired
 * mid-frame and truncated the bottom slices (bottom-half-corruption RCA,
 * 2026-07-05). In trailer streams au_asm emits on the frame trailer, so this
 * rarely fires at all. 0 disables it entirely (pure au-start trigger,
 * avformat-equivalent). Override via env PIVID_PLAYER_IDLE_FLUSH_MS. */
#define LAN_SOURCE_IDLE_FLUSH_MS_DEFAULT 250
#define LAN_SOURCE_IDLE_FLUSH_MS_MAX     500

typedef struct {
    uint8_t buf[AU_ASM_CAP];
    size_t  len;
#ifdef LATPROBE
    uint64_t ingress_ns;   /* recv ts of the datagram that completed this AU */
#endif
} LanAuSlot;

typedef struct {
    int          fd;
    atomic_bool *stop;
    LanReasm     reasm;
    AuAsm        auasm;
    LanAuSlot    au[LAN_SOURCE_AU_SLOTS];    /* ring of completed AUs */
    int          au_head, au_tail, au_count;
    uint64_t     au_overrun;                 /* ring full -> dropped oldest */
    uint64_t     idle_flush_ns;              /* timeout-flush threshold (0=off) */
    uint64_t     last_data_ns;               /* mono ts of the last datagram fed */
#ifdef LATPROBE
    uint64_t     last_recv_ns;               /* recv ts of the current datagram */
    uint64_t     au_ingress_ns;              /* ingress ts of the last popped AU */
#endif
} LanSource;

/* rc for lan_source_next_au. */
#define LAN_SOURCE_OK    0
#define LAN_SOURCE_STOP  1

int  lan_source_open(LanSource *s, uint16_t port, atomic_bool *stop, uint16_t *out_port);

/* Block until one complete AU is available; on return LAN_SOURCE_OK, *au and
 * *len point into s (valid until the next lan_source_next_au call -- copy to
 * keep). Returns LAN_SOURCE_STOP when stop is set or on a fatal socket error. */
int  lan_source_next_au(LanSource *s, const uint8_t **au, size_t *len);

void lan_source_close(LanSource *s);

/* --- socket-free core (unit-testable; used by next_au and the tests) ------ */
void lan_source_reset(LanSource *s);                       /* wire reasm+auasm cbs */
void lan_source_feed_datagram(LanSource *s, const uint8_t *dg, size_t dg_len, uint64_t now_ns);
/* recv-timeout handler: always ticks reasm (its own 150ms stale flush is
 * independent); presents the buffered AU early only if idle_flush_ns is set and
 * the wire has been silent >= idle_flush_ns since the last datagram. */
void lan_source_on_timeout(LanSource *s, uint64_t now_ns);
int  lan_source_pop_au(LanSource *s, const uint8_t **au, size_t *len); /* 1 if popped */

#ifdef LATPROBE
/* Ingress (recv) timestamp of the AU returned by the most recent pop/next_au. */
uint64_t lan_source_cur_ingress_ns(const LanSource *s);
#endif

#endif
