#include "pkt_hist.h"
#include <stdio.h>
#include <unistd.h>

/* Airtime model (spec §Problem): info rate after CR 3/4 ~= 1.95 Mbps =>
 * 4.10 us/byte; fixed framing ~400 us/packet; dynamic adds ~48 us/packet
 * (8 us header + ~40 us per-packet SetFlrcPacketParams reprogram). Fixed
 * always transmits 511 payload bytes. */
#define US_PER_BYTE   4.10
#define FRAMING_US    400.0
#define DYN_OVERHEAD  48.0
#define FIXED_PLD     511.0

void pkt_hist_init(PktHist *h) {
    h->count = 0;
    h->total_bytes = 0;
    h->near_full = 0;
    for (int i = 0; i < PKT_HIST_BUCKETS; i++) h->bucket[i] = 0;
}

void pkt_hist_add(PktHist *h, size_t len) {
    int b = (int)(len / 32);
    if (b >= PKT_HIST_BUCKETS) b = PKT_HIST_BUCKETS - 1;
    h->count++;
    h->total_bytes += (uint64_t)len;
    if (len >= PKT_HIST_NEAR_FULL) h->near_full++;
    h->bucket[b]++;
}

PktHistDecision pkt_hist_decide(const PktHist *h) {
    PktHistDecision d = {0, 0.0, 0.0, 0.0};
    if (h->count == 0) { d.recommend_fixed = 1; return d; }

    /* near_full_frac: exact fraction of packets with len >= PKT_HIST_NEAR_FULL,
     * tracked in pkt_hist_add (the 32-byte buckets cannot express >=499 exactly,
     * so the decision record uses the exact counter). */
    d.near_full_frac = (double)h->near_full / (double)h->count;

    /* Fixed: every packet pays full 511-byte airtime + framing. */
    d.fixed_airtime_us = (double)h->count * (FIXED_PLD * US_PER_BYTE + FRAMING_US);
    /* Dynamic: each packet pays its real bytes + framing + per-packet overhead. */
    d.dyn_airtime_us = (double)h->total_bytes * US_PER_BYTE
                     + (double)h->count * (FRAMING_US + DYN_OVERHEAD);

    d.recommend_fixed = (d.fixed_airtime_us <= d.dyn_airtime_us) ? 1 : 0;
    return d;
}

void pkt_hist_dump(const PktHist *h, int fd) {
    char line[160];
    for (int b = 0; b < PKT_HIST_BUCKETS; b++) {
        if (h->bucket[b] == 0) continue;
        int n = snprintf(line, sizeof line, "pkt_hist bucket %3d-%3d: %llu\n",
                         b * 32, b * 32 + 31, (unsigned long long)h->bucket[b]);
        if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }
    }
    PktHistDecision d = pkt_hist_decide(h);
    int n = snprintf(line, sizeof line,
        "pkt_hist DECISION: recommend=%s near_full_frac=%.3f "
        "fixed_us=%.0f dyn_us=%.0f (count=%llu bytes=%llu)\n",
        d.recommend_fixed ? "FIXED-511" : "DYNAMIC",
        d.near_full_frac, d.fixed_airtime_us, d.dyn_airtime_us,
        (unsigned long long)h->count, (unsigned long long)h->total_bytes);
    if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }
}
