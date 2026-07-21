/* apps/pivid-player/src/lan_reasm.h -- reassemble LAN-hop video chunks into
 * framed NALs. Pure state machine (no sockets); mirrors apps/pivid-rx/src/
 * reasm.c but addresses chunks by byte offset so out-of-order delivery and
 * partial-forward both work. On flush it emits the clean contiguous prefix
 * [0..first_gap) of each NAL in ascending nal_id order; the player feeds that to
 * the SW conceal-in-place decoder. */
#ifndef PLAYER_LAN_REASM_H
#define PLAYER_LAN_REASM_H

#include <stddef.h>
#include <stdint.h>
#include "lanfrag.h"

#define LAN_REASM_SLOTS       4
#define LAN_REASM_TIMEOUT_NS  150000000ull  /* 150 ms backstop for a burst tail */

/* A chunk for a nal_id within this many ids *behind* the last-emitted id is a
 * stale straggler/duplicate and is dropped. The window is small and deliberately
 * NOT the full 32768-id past: the caster resets nal_id to 0 on restart, and
 * treating the whole backward half-space as stale would black out the player for
 * thousands of NALs until the id climbed back. A jump older than the window is
 * treated as a stream restart and accepted (re-bases last_emitted). 16 >> real
 * LAN reordering depth (~1-2) and the 4-slot in-flight window. */
#define LAN_REASM_STALE_WINDOW 16

/* emit: a flushed framed NAL prefix (len may be < total on partial loss; len==0
 * is never emitted). Valid only for the call; copy to retain. */
typedef void (*LanReasm_EmitCb)(const uint8_t *nal, size_t len, void *user);

typedef struct {
    int      used;
    uint16_t nal_id;
    uint16_t total_len;
    uint64_t recv_mask;                 /* bit i set => chunk i (off i*1400) in   */
    uint64_t last_ns;
    uint8_t  buf[LANFRAG_MAX_NAL];
} LanSlot;

typedef struct {
    LanSlot         slots[LAN_REASM_SLOTS];
    LanReasm_EmitCb emit;
    void           *user;
    uint16_t        last_emitted;       /* nal_id of the most recently flushed NAL */
    int             have_emitted;
    /* counters */
    uint64_t        nals_complete;
    uint64_t        nals_partial;
    uint64_t        nal_gaps;           /* whole NALs missing from the caster nal_id
                                         * sequence -- invisible to nals_partial (no
                                         * slot ever opened) => upstream/wire loss of
                                         * an entire slice NAL                      */
    uint64_t        chunks_dropped;     /* stale/duplicate/invalid-geometry chunks */
    uint64_t        nal_degenerate;     /* total_len==0 header (parsed but empty)    */
    uint64_t        bad_magic;          /* parse failure (magic/version/short)      */
} LanReasm;

void lan_reasm_init(LanReasm *r, LanReasm_EmitCb emit, void *user);

/* Feed one received UDP datagram (header + payload). now_ns drives the timeout
 * and supersede bookkeeping. */
void lan_reasm_feed(LanReasm *r, const uint8_t *dg, size_t dg_len, uint64_t now_ns);

/* Flush any slot idle longer than LAN_REASM_TIMEOUT_NS (+ all older slots),
 * ascending. Call once per read loop iteration. */
void lan_reasm_tick(LanReasm *r, uint64_t now_ns);

/* Flush every open slot in ascending order (shutdown / EOF). */
void lan_reasm_flush_all(LanReasm *r);

#endif
