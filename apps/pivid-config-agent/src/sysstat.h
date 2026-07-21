#ifndef AGENT_SYSSTAT_H
#define AGENT_SYSSTAT_H
#include <stdint.h>

/* /proc/stat first ("cpu ") line, split into the jiffy fields. */
typedef struct { uint64_t user, nice, system, idle, iowait, irq, softirq, steal; } CpuTimes;

/* Parse the "cpu " aggregate line from /proc/stat content. Returns 0 / -1. */
int  sysstat_parse_cpu(const char *proc_stat, CpuTimes *out);
/* Busy% over [prev, cur] as 0..100 (0 if the interval is empty). */
int  sysstat_cpu_busy_pct(const CpuTimes *prev, const CpuTimes *cur);
/* Used-RAM% from /proc/meminfo content (MemTotal/MemAvailable). -1 on parse fail. */
int  sysstat_mem_used_pct(const char *proc_meminfo);
/* Milli-degree thermal_zone value -> whole degrees C. -1 on parse fail. */
int  sysstat_thermal_c(const char *thermal_raw);
/* Stream uptime (whole seconds) = system uptime - daemon process start time.
 * proc_uptime is /proc/uptime content (first field = seconds, float);
 * proc_pid_stat is the daemon's /proc/<pid>/stat content (field 22 = starttime
 * in clock ticks); clk_tck is sysconf(_SC_CLK_TCK). Robust against a comm that
 * contains spaces or parentheses. Clamps a negative result (clock skew) to 0;
 * returns -1 on parse failure or clk_tck <= 0. */
long sysstat_stream_uptime_secs(const char *proc_uptime,
                                const char *proc_pid_stat, long clk_tck);
/* Parse the bitmask out of `vcgencmd get_throttled` output ("throttled=0x...").
 * Returns the value (>=0) or -1 if no hex field is present. Bit meanings:
 * 0=under-voltage now, 1=freq capped now, 2=throttled now, 3=soft-temp now;
 * 16..19 = the same conditions have occurred since boot (sticky). */
long sysstat_throttle_bits(const char *get_throttled);
#endif
