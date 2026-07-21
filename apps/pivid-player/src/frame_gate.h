/* apps/pivid-player/src/frame_gate.h -- a waitable generation counter.
 *
 * A producer bumps `generation` (under `lock`) and signals `cond`; a consumer
 * blocks in frame_gate_wait until the generation moves off a value it has already
 * seen, or a relative timeout elapses. Pure pthread (no libav) so it is unit-
 * testable in isolation. The producer side (bump+signal) is inlined by
 * frame_slot_publish while it holds `lock` -- kept atomic with the frame swap --
 * so this exposes no separate "bump" entry point. */
#ifndef PLAYER_FRAME_GATE_H
#define PLAYER_FRAME_GATE_H
#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    clockid_t       clock;        /* clock domain of `cond` (set in init)      */
    uint64_t        generation;   /* bumped on each publish                     */
} FrameGate;

void frame_gate_init(FrameGate *g);
void frame_gate_destroy(FrameGate *g);   /* call only after the producer joined */

/* Block until `generation != seen_gen` or `timeout_ns` elapses (RELATIVE to the
 * call; <= 0 => return immediately without blocking). Lost-wakeup-safe: the
 * predicate is re-checked under `lock` on every wake. Returns the current
 * generation (== seen_gen only when it timed out with no intervening publish). */
uint64_t frame_gate_wait(FrameGate *g, uint64_t seen_gen, int64_t timeout_ns);

#endif
