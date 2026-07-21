/* Pure throughput helper shared by the daemons + host tests. kbps = bits/sec/1000
 * from a byte delta over an elapsed-ms window. */
#ifndef COMMON_KBPS_H
#define COMMON_KBPS_H

#include <stdint.h>

/* bytes over elapsed_ms -> kbps. elapsed_ms <= 0 yields 0 (first-sample guard). */
static inline uint64_t kbps_from_delta(uint64_t bytes, int64_t elapsed_ms) {
    if (elapsed_ms <= 0) return 0;
    return (bytes * 8ull) / (uint64_t)elapsed_ms;   /* bytes*8 / ms == bits/ms == kbps */
}

/* From two cumulative counters; a non-increasing counter (restart) yields 0. */
static inline uint64_t kbps_from_delta_counters(uint64_t prev, uint64_t now,
                                                int64_t elapsed_ms) {
    if (now < prev) return 0;
    return kbps_from_delta(now - prev, elapsed_ms);
}

#endif
