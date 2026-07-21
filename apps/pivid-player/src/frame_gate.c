#define _POSIX_C_SOURCE 200809L
#include "frame_gate.h"
#include <errno.h>

void frame_gate_init(FrameGate *g) {
    pthread_mutex_init(&g->lock, NULL);
#if defined(__linux__)
    /* Pin `cond` to CLOCK_MONOTONIC so waits are immune to wall-clock steps (NTP)
     * -- important for the RX-local player on the Pi. */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g->cond, &attr);
    pthread_condattr_destroy(&attr);
    g->clock = CLOCK_MONOTONIC;
#else
    /* macOS has no pthread_condattr_setclock; pthread_cond_timedwait uses the
     * realtime clock, so compute the deadline from CLOCK_REALTIME to match. */
    pthread_cond_init(&g->cond, NULL);
    g->clock = CLOCK_REALTIME;
#endif
    g->generation = 0;
}

void frame_gate_destroy(FrameGate *g) {
    pthread_cond_destroy(&g->cond);
    pthread_mutex_destroy(&g->lock);
}

uint64_t frame_gate_wait(FrameGate *g, uint64_t seen, int64_t timeout_ns) {
    pthread_mutex_lock(&g->lock);
    if (timeout_ns > 0 && g->generation == seen) {
        struct timespec now;
        clock_gettime(g->clock, &now);
        int64_t deadline_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + timeout_ns;
        struct timespec deadline = { .tv_sec  = (time_t)(deadline_ns / 1000000000LL),
                                     .tv_nsec = (long)  (deadline_ns % 1000000000LL) };
        /* Break on timeout OR any (unexpected) error -- never busy-spin re-waiting. */
        while (g->generation == seen)
            if (pthread_cond_timedwait(&g->cond, &g->lock, &deadline) != 0) break;
    }
    uint64_t cur = g->generation;
    pthread_mutex_unlock(&g->lock);
    return cur;
}
