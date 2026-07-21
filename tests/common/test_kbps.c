#include "../common/kbps.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* 125000 bytes in 1000 ms = 1,000,000 bits/s = 1000 kbps. */
    assert(kbps_from_delta(125000, 1000) == 1000);
    /* zero / negative elapsed -> 0 (avoid div by zero on first sample) */
    assert(kbps_from_delta(125000, 0) == 0);
    /* tiny forward delta: (100-50)*8/1000 = 0 by integer truncation */
    assert(kbps_from_delta_counters(50, 100, 1000) == 0);
    /* counter went backwards (daemon restart) -> 0 (now < prev guard) */
    assert(kbps_from_delta_counters(100, 50, 1000) == 0);
    /* normal delta: (1000-0) bytes in 8 ms = 1000*8/8 = 1000 kbps */
    assert(kbps_from_delta_counters(0, 1000, 8) == 1000);
    printf("test_kbps: OK\n");
    return 0;
}
