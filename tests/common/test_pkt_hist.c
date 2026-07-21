#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../common/pkt_hist.h"

/* Buckets are 32 bytes wide: [0..31],[32..63],...,[480..511],[512+ overflow]. */
static void test_add_and_bucket(void) {
    PktHist h;
    pkt_hist_init(&h);
    pkt_hist_add(&h, 10);    /* bucket 0 */
    pkt_hist_add(&h, 253);   /* bucket 7  (253/32 = 7) */
    pkt_hist_add(&h, 500);   /* bucket 15 (500/32 = 15) */
    pkt_hist_add(&h, 511);   /* bucket 15 */
    assert(h.count == 4);
    assert(h.total_bytes == 10 + 253 + 500 + 511);
    assert(h.bucket[0] == 1);
    assert(h.bucket[7] == 1);
    assert(h.bucket[15] == 2);
}

/* The decision metric: fraction of PACKETS that are "near-full" (>= 499 B),
 * and airtime under each scheme. See pkt_hist_decide(). */
static void test_decide_favours_fixed_when_all_full(void) {
    PktHist h;
    pkt_hist_init(&h);
    for (int i = 0; i < 1000; i++) pkt_hist_add(&h, 505);
    PktHistDecision d = pkt_hist_decide(&h);
    /* All packets near-full -> fixed-511 padding is cheap -> fixed wins. */
    assert(d.recommend_fixed == 1);
    assert(d.near_full_frac > 0.99);
}

static void test_decide_favours_dynamic_with_small_packets(void) {
    PktHist h;
    pkt_hist_init(&h);
    for (int i = 0; i < 500; i++) pkt_hist_add(&h, 505); /* full */
    for (int i = 0; i < 500; i++) pkt_hist_add(&h, 60);  /* small */
    PktHistDecision d = pkt_hist_decide(&h);
    /* Half the packets waste ~1.8 ms each under fixed-511 padding. */
    assert(d.recommend_fixed == 0);
}

int main(void) {
    test_add_and_bucket();
    test_decide_favours_fixed_when_all_full();
    test_decide_favours_dynamic_with_small_packets();
    printf("test_pkt_hist: all passed\n");
    return 0;
}
