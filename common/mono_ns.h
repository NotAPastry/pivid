#ifndef COMMON_MONO_NS_H
#define COMMON_MONO_NS_H

#include <stdint.h>
#include <time.h>

/* Monotonic nanosecond clock shared by the latency probes (S1-S8).
 * CLOCK_MONOTONIC so an NTP step cannot corrupt a delta. Fine on macOS 10.12+. */
static inline uint64_t pv_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif
