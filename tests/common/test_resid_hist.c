#include "resid_hist.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MS 1000000ull

static const uint64_t COARSE[] = {
    500000, 1000000, 2000000, 4000000, 8000000, 16000000,
    24000000, 32000000, 40000000, 50000000, 70000000, (uint64_t)-1 };

static void test_add_and_bucket(void) {
    ResidHist h;
    resid_hist_init(&h, "s_test", COARSE, 12, 5ull * MS);
    resid_hist_add(&h, 100000ull);    /* 0.1 ms -> bucket 0            */
    resid_hist_add(&h, 3ull * MS);    /* 3 ms   -> bucket 3 (<4)       */
    resid_hist_add(&h, 33ull * MS);   /* 33 ms  -> bucket 8 (<40)      */
    resid_hist_add(&h, 100ull * MS);  /* 100 ms -> bucket 11 (overflow)*/
    assert(h.count == 4);
    assert(h.bucket[0] == 1);
    assert(h.bucket[3] == 1);
    assert(h.bucket[8] == 1);
    assert(h.bucket[11] == 1);
    assert(h.max_ns == 100ull * MS);
}

static void test_held_and_mean(void) {
    ResidHist h;
    resid_hist_init(&h, "s_test", COARSE, 12, 5ull * MS);
    resid_hist_add(&h, 1ull * MS);    /* below held threshold */
    resid_hist_add(&h, 10ull * MS);   /* held */
    resid_hist_add(&h, 30ull * MS);   /* held */
    assert(h.count == 3);
    assert(h.held == 2);
    assert(h.held_ns_sum == 40ull * MS);   /* mean_held = 20 ms */
}

static void test_edge_boundary_is_exclusive_upper(void) {
    /* A value exactly on an edge falls in the NEXT bucket (scan is >=). */
    ResidHist h;
    resid_hist_init(&h, "s_test", COARSE, 12, 5ull * MS);
    resid_hist_add(&h, 500000ull);    /* == edge[0]; not < 0.5ms -> bucket 1 */
    assert(h.bucket[0] == 0);
    assert(h.bucket[1] == 1);
}

static void test_clamp_n_buckets(void) {
    /* n_buckets past the fixed array is clamped to the max (the one genuinely
     * new behavior over the fixed-size siblings). */
    uint64_t big[20];
    for (int i = 0; i < 20; i++) big[i] = (uint64_t)(i + 1) * MS;
    big[19] = (uint64_t)-1;
    ResidHist h;
    resid_hist_init(&h, "s_test", big, RESID_HIST_MAX_BUCKETS + 4, 5ull * MS);
    assert(h.n_buckets == RESID_HIST_MAX_BUCKETS);
}

static void test_fine_edge_set(void) {
    /* A different edge set + n than COARSE: exercises b == n_buckets-1
     * (last/overflow bucket) detection at n=11 with distinct edges. */
    static const uint64_t FINE[] = {
        100000, 250000, 500000, 1000000, 2000000, 4000000,
        8000000, 16000000, 24000000, 33000000, (uint64_t)-1 };
    ResidHist h;
    resid_hist_init(&h, "s_test", FINE, 11, 5ull * MS);
    resid_hist_add(&h, 50ull * MS);   /* >= 33ms -> last (overflow) bucket 10 */
    resid_hist_add(&h, 50000ull);     /* 0.05 ms (< 0.1ms edge) -> bucket 0   */
    assert(h.bucket[10] == 1);
    assert(h.bucket[0] == 1);
}

static void test_dump_smoke(void) {
    /* dump() must not crash and must emit a SUMMARY line. Also dump an all-zero
     * (count==0) histogram to confirm the mean/pct guards are divide-by-zero-safe
     * and it still summarizes. */
    char buf[512];
    int fds[2];

    ResidHist h;
    resid_hist_init(&h, "s_test", COARSE, 12, 5ull * MS);
    resid_hist_add(&h, 10ull * MS);
    assert(pipe(fds) == 0);
    resid_hist_dump(&h, fds[1]);
    close(fds[1]);
    ssize_t r = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    assert(r > 0);
    buf[r] = '\0';
    assert(strstr(buf, "SUMMARY") != NULL);

    ResidHist empty;
    resid_hist_init(&empty, "s_test", COARSE, 12, 5ull * MS);
    assert(pipe(fds) == 0);
    resid_hist_dump(&empty, fds[1]);   /* count==0 path */
    close(fds[1]);
    ssize_t r2 = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    assert(r2 > 0);
    buf[r2] = '\0';
    assert(strstr(buf, "SUMMARY") != NULL);
}

int main(void) {
    test_add_and_bucket();
    test_held_and_mean();
    test_edge_boundary_is_exclusive_upper();
    test_clamp_n_buckets();
    test_fine_edge_set();
    test_dump_smoke();
    printf("test_resid_hist: all passed\n");
    return 0;
}
