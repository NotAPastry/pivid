#include <assert.h>
#include <stdio.h>
#include "../apps/pivid-config-agent/src/sysstat.h"

int main(void) {
    CpuTimes a, b;
    assert(sysstat_parse_cpu("cpu  100 0 50 1000 0 0 0 0\ncpu0 ...\n", &a) == 0);
    assert(a.user == 100 && a.system == 50 && a.idle == 1000);
    /* +100 busy (user), +100 idle over the interval -> 50% */
    assert(sysstat_parse_cpu("cpu  200 0 50 1100 0 0 0 0\n", &b) == 0);
    assert(sysstat_cpu_busy_pct(&a, &b) == 50);

    assert(sysstat_mem_used_pct("MemTotal: 1000 kB\nMemAvailable: 250 kB\n") == 75);
    assert(sysstat_thermal_c("48312\n") == 48);
    assert(sysstat_thermal_c("garbage") == -1);

    /* stream uptime: system uptime - daemon process start (field 22 of
     * /proc/<pid>/stat, in clock ticks). Realistic line: starttime 882286
     * ticks @ 100 Hz = 8822.86 s; uptime 12283.54 s -> ~3461 s. */
    const char *stat1 = "2375 (pivid-tx) S 2373 2373 2373 0 -1 1077936128 "
        "1234 0 0 0 100 50 0 0 20 0 1 0 882286 12345678 0";
    long up = sysstat_stream_uptime_secs("12283.54 9999.99\n", stat1, 100);
    assert(up >= 3460 && up <= 3461);

    /* comm containing spaces and parens must not break field 22 parsing. */
    const char *stat2 = "7 (weird ) name) R 11 12 13 14 15 16 17 18 19 20 "
        "21 22 23 24 25 26 27 28 4000 555 666";
    assert(sysstat_stream_uptime_secs("100.0 50.0\n", stat2, 100) == 60); /* 100 - 4000/100 */

    /* clamp: process start after system uptime (clock skew) -> 0, never negative. */
    assert(sysstat_stream_uptime_secs("100.0 0\n",
        "9 (d) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 20000 0", 100) == 0);

    /* throttle bits: parse the hex value out of `vcgencmd get_throttled`. */
    assert(sysstat_throttle_bits("throttled=0x0\n") == 0);
    assert(sysstat_throttle_bits("throttled=0x50005\n") == 0x50005);
    assert(sysstat_throttle_bits("0xf") == 0xf);
    assert(sysstat_throttle_bits("no hex here") == -1);
    assert(sysstat_throttle_bits("") == -1);

    /* guards: bad clk_tck and malformed stat -> -1. */
    assert(sysstat_stream_uptime_secs("12283.54\n", stat1, 0) == -1);
    assert(sysstat_stream_uptime_secs("12283.54\n", "no parens here", 100) == -1);
    assert(sysstat_stream_uptime_secs("garbage", stat1, 100) == -1);

    printf("All sysstat tests passed!\n");
    return 0;
}
