#ifndef COMMON_PKT_HIST_H
#define COMMON_PKT_HIST_H

#include <stddef.h>
#include <stdint.h>

/* 16 buckets of 32 bytes cover 0..511; bucket 16 catches >=512 (should be 0). */
#define PKT_HIST_BUCKETS 17
#define PKT_HIST_NEAR_FULL 499   /* fixed/dynamic crossover from the spec */

typedef struct {
    uint64_t count;
    uint64_t total_bytes;
    uint64_t near_full;       /* packets with len >= PKT_HIST_NEAR_FULL */
    uint64_t bucket[PKT_HIST_BUCKETS];
} PktHist;

typedef struct {
    int    recommend_fixed;   /* 1 = fixed-511 default, 0 = dynamic default */
    double near_full_frac;    /* fraction of packets >= PKT_HIST_NEAR_FULL  */
    double fixed_airtime_us;  /* modelled total airtime under fixed-511     */
    double dyn_airtime_us;    /* modelled total airtime under dynamic       */
} PktHistDecision;

void pkt_hist_init(PktHist *h);
void pkt_hist_add(PktHist *h, size_t len);
PktHistDecision pkt_hist_decide(const PktHist *h);
/* Human-readable dump (one line per non-empty bucket + the decision). */
void pkt_hist_dump(const PktHist *h, int fd);

#endif
