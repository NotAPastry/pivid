#ifndef COMMON_RESID_HIST_H
#define COMMON_RESID_HIST_H

#include <stddef.h>
#include <stdint.h>

/* Generalized residency histogram (sizes the latency-budget stages S1-S8).
 *
 * A "residency" is a monotonic-ns delta (time a unit of work spent held at a
 * pipeline stage). This module only accumulates + dumps; it changes nothing.
 * Always cheap: one linear bucket scan per add. Zero output until a dump.
 *
 * Generalized from the original nal_resid: bucket edges, held-threshold, and a
 * dump label are now per-instance so one module serves every stage. */

#define RESID_HIST_MAX_BUCKETS 16

typedef struct {
    const char *label;                          /* dump prefix, e.g. "s2_fec_tx" */
    int         n_buckets;                      /* active edges (<= MAX)          */
    uint64_t    edge_ns[RESID_HIST_MAX_BUCKETS];/* ascending upper edges; last = UINT64_MAX */
    uint64_t    held_ns;                        /* "held" threshold               */
    uint64_t    count;
    uint64_t    held;                           /* residency >= held_ns           */
    uint64_t    held_ns_sum;                    /* sum of held residencies (mean) */
    uint64_t    max_ns;
    uint64_t    bucket[RESID_HIST_MAX_BUCKETS];
} ResidHist;

/* edges: ascending upper-edge ns array of length n_buckets; caller ensures the
 * last entry is UINT64_MAX (the overflow bucket). n_buckets is clamped to MAX. */
void resid_hist_init(ResidHist *h, const char *label,
                     const uint64_t *edges, int n_buckets, uint64_t held_ns);
void resid_hist_add(ResidHist *h, uint64_t resid_ns);
void resid_hist_dump(const ResidHist *h, int fd);

#endif
