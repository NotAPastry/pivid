#include "resid_hist.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void resid_hist_init(ResidHist *h, const char *label,
                     const uint64_t *edges, int n_buckets, uint64_t held_ns) {
    memset(h, 0, sizeof *h);
    if (n_buckets > RESID_HIST_MAX_BUCKETS) n_buckets = RESID_HIST_MAX_BUCKETS;
    h->label     = label;
    h->n_buckets = n_buckets;
    h->held_ns   = held_ns;
    for (int i = 0; i < n_buckets; i++) h->edge_ns[i] = edges[i];
}

void resid_hist_add(ResidHist *h, uint64_t resid_ns) {
    int b = 0;
    while (b < h->n_buckets - 1 && resid_ns >= h->edge_ns[b]) b++;
    h->bucket[b]++;
    h->count++;
    if (resid_ns >= h->held_ns) { h->held++; h->held_ns_sum += resid_ns; }
    if (resid_ns > h->max_ns) h->max_ns = resid_ns;
}

void resid_hist_dump(const ResidHist *h, int fd) {
    char line[160];
    double lo = 0.0;
    for (int b = 0; b < h->n_buckets; b++) {
        double hi = (h->edge_ns[b] == (uint64_t)-1) ? 0.0 : h->edge_ns[b] / 1e6;
        int n;
        if (h->bucket[b] == 0) { lo = hi; continue; }
        if (b == h->n_buckets - 1)
            n = snprintf(line, sizeof line, "%s bucket >=%.1fms: %llu\n",
                         h->label, lo, (unsigned long long)h->bucket[b]);
        else
            n = snprintf(line, sizeof line, "%s bucket %.1f-%.1fms: %llu\n",
                         h->label, lo, hi, (unsigned long long)h->bucket[b]);
        if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }
        lo = hi;
    }
    double mean_held = h->held ? (double)h->held_ns_sum / (double)h->held / 1e6 : 0.0;
    double pct = h->count ? 100.0 * (double)h->held / (double)h->count : 0.0;
    int n = snprintf(line, sizeof line,
        "%s SUMMARY: count=%llu held(>=%llums)=%llu (%.1f%%) mean_held=%.1fms max=%.1fms\n",
        h->label,
        (unsigned long long)h->count,
        (unsigned long long)(h->held_ns / 1000000ull),
        (unsigned long long)h->held, pct, mean_held, h->max_ns / 1e6);
    if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }
}
