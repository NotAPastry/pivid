#define _POSIX_C_SOURCE 200809L
/* apps/pivid-player/src/frame_slot.c -- see frame_slot.h. */
#include "frame_slot.h"
#include <libavutil/frame.h>

void frame_slot_init(FrameSlot *s) {
    frame_gate_init(&s->gate);
    s->frame = NULL;
    s->arrival_ns = 0;
}

void frame_slot_destroy(FrameSlot *s) {
    if (s->frame) av_frame_free(&s->frame);
    frame_gate_destroy(&s->gate);
}

void frame_slot_publish(FrameSlot *s, AVFrame *src, int64_t arrival_ns) {
    /* Build the new ref OUTSIDE the lock. */
    AVFrame *nf = av_frame_alloc();
    if (!nf) return;
    if (av_frame_ref(nf, src) < 0) { av_frame_free(&nf); return; }

    pthread_mutex_lock(&s->gate.lock);
    AVFrame *old = s->frame;
    s->frame = nf;
    s->arrival_ns = arrival_ns;
    s->gate.generation++;                 /* bump + signal atomically with the swap */
    pthread_cond_signal(&s->gate.cond);   /* wake a render loop blocked in frame_slot_wait */
    pthread_mutex_unlock(&s->gate.lock);

    if (old) av_frame_free(&old);         /* free displaced frame outside the lock */
}

bool frame_slot_take(FrameSlot *s, AVFrame *dst, int64_t *out_ns, uint64_t *out_gen) {
    bool ok = false;
    pthread_mutex_lock(&s->gate.lock);
    if (s->frame) {
        av_frame_unref(dst);
        if (av_frame_ref(dst, s->frame) == 0) {
            *out_ns = s->arrival_ns;
            *out_gen = s->gate.generation;
            ok = true;
        }
    }
    pthread_mutex_unlock(&s->gate.lock);
    return ok;
}

uint64_t frame_slot_wait(FrameSlot *s, uint64_t seen_gen, int64_t timeout_ns) {
    return frame_gate_wait(&s->gate, seen_gen, timeout_ns);
}
