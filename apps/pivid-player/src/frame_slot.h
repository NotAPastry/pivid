/* apps/pivid-player/src/frame_slot.h -- single-slot latest-wins AVFrame holder.
 *
 * The decode thread publishes the newest frame; the render thread takes the
 * latest. A newer frame overwrites (drop, never queue). Frames are ref-counted
 * (av_frame_ref), never deep-copied; the gate lock is held ONLY for the pointer
 * swap, so the GPU upload in the render thread happens outside the lock. The
 * embedded FrameGate also lets the render loop BLOCK until a new frame is
 * published (IMP-7) instead of polling. */
#ifndef PLAYER_FRAME_SLOT_H
#define PLAYER_FRAME_SLOT_H
#include "frame_gate.h"
#include <stdbool.h>
#include <stdint.h>
struct AVFrame;

typedef struct {
    FrameGate        gate;         /* lock + cond + generation (waitable)        */
    struct AVFrame  *frame;        /* owned; NULL until first publish            */
    int64_t          arrival_ns;   /* monotonic ts of the held frame            */
} FrameSlot;

void frame_slot_init(FrameSlot *s);
/* Free the held frame + destroy the gate. CONTRACT: call only after the
 * producer (decode) thread has been joined -- destroy takes no lock. */
void frame_slot_destroy(FrameSlot *s);

/* Publish `src` as the new latest (the slot takes a ref via av_frame_ref into
 * its own frame, unref-ing the displaced one). `arrival_ns` is the decode clock.
 * Bumps the gate generation + signals it so a blocked render loop wakes now. */
void frame_slot_publish(FrameSlot *s, struct AVFrame *src, int64_t arrival_ns);

/* If a frame is present, ref it into `dst` (caller-owned, must be allocated) and
 * return its arrival_ns via *out_ns + generation via *out_gen; returns true.
 * Returns false (no copy) when the slot is empty. The render thread then uploads
 * `dst` to the GPU with NO lock held. */
bool frame_slot_take(FrameSlot *s, struct AVFrame *dst,
                     int64_t *out_ns, uint64_t *out_gen);

/* Block until a generation newer than `seen_gen` is published or `timeout_ns`
 * elapses (relative; <= 0 => return immediately). Passthrough to frame_gate_wait;
 * the return value is the current generation (usually ignored -- the caller
 * re-takes the latest frame). */
uint64_t frame_slot_wait(FrameSlot *s, uint64_t seen_gen, int64_t timeout_ns);

#endif
