/* apps/pivid-player/src/player_clock.h -- shared monotonic clock helper.
 * Header-only static inline so each TU gets it with zero link cost. */
#ifndef PLAYER_CLOCK_H
#define PLAYER_CLOCK_H
#include <stdint.h>
#include <time.h>
static inline int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#endif
