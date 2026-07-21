#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../common/link_capacity.h"

/* flrc_cr: 1 = 3/4 chip FEC (default), 2 = 1/1 (none). */
#define CR_34   1
#define CR_11   2

static int near_u(uint64_t a, uint64_t b, uint64_t tol) {
    uint64_t d = (a > b) ? a - b : b - a;
    return d <= tol;
}

/* Calibration anchor 1: cr 3/4, 511 B DYNAMIC, no software FEC. Anchored to
 * the measured TXPERF period 2446 us (2026-07-05 IMP-2: fast BUSY-wait +
 * 16 us PA ramp) -> ~1.67 Mbps. */
static void test_anchor_cr34_511(void) {
    uint64_t air = 0, video = 0;
    link_capacity(CR_34, 511, 0, 16, &air, &video);
    assert(near_u(air, 1671, 3));   /* dynamic cr3/4 full-packet ceiling */
    assert(video == air);           /* m=0 -> all of the ceiling is video */
}

/* Calibration anchor 2: cr 1/1, 511 B DYNAMIC, no software FEC (predicted
 * from the cr3/4 anchor; cr1/1 is not bench-measurable -- live cr change
 * wedges the chip and CR 3/4 is Locked). */
static void test_anchor_cr11_511(void) {
    uint64_t air = 0, video = 0;
    link_capacity(CR_11, 511, 0, 16, &air, &video);
    assert(near_u(air, 2127, 3));   /* dynamic cr1/1 full-packet ceiling */
    assert(video == air);
}

/* Software FEC m/k carves the video budget out of the air ceiling. */
static void test_software_fec_budget(void) {
    uint64_t air = 0, video = 0;
    link_capacity(CR_34, 511, 2, 16, &air, &video);
    assert(near_u(air, 1671, 3));
    /* video == air * k/(k+m) == air * 16/18 */
    assert(near_u(video, air * 16 / 18, 2));
    assert(video < air);
}

/* Guard: invalid flrc_cr (0, 3) falls back to cr 3/4. */
static void test_guard_invalid_cr(void) {
    uint64_t a0 = 0, a3 = 0, v = 0, ref = 0;
    link_capacity(CR_34, 511, 0, 16, &ref, &v);
    link_capacity(0, 511, 0, 16, &a0, &v);
    link_capacity(3, 511, 0, 16, &a3, &v);
    assert(a0 == ref);
    assert(a3 == ref);
}

/* Guard: fec_k=0 must not divide-by-zero; behaves as k=1. */
static void test_guard_fec_k_zero(void) {
    uint64_t air = 0, v0 = 0, v1 = 0;
    link_capacity(CR_34, 511, 2, 0, &air, &v0); /* k=0 -> k=1 */
    link_capacity(CR_34, 511, 2, 1, &air, &v1);
    assert(v0 == v1);               /* k=0 treated as k=1            */
    assert(near_u(v0, air * 1 / 3, 2)); /* air * 1/(1+2)             */
}

/* Guard: negative fec_m behaves as m=0 (video == air). */
static void test_guard_fec_m_negative(void) {
    uint64_t air = 0, video = 0;
    link_capacity(CR_34, 511, -5, 16, &air, &video);
    assert(video == air);
}

int main(void) {
    test_anchor_cr34_511();
    test_anchor_cr11_511();
    test_software_fec_budget();
    test_guard_invalid_cr();
    test_guard_fec_k_zero();
    test_guard_fec_m_negative();
    printf("test_link_capacity: all passed\n");
    return 0;
}
