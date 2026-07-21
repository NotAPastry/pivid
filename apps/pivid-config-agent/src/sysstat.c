#define _POSIX_C_SOURCE 200809L
#include "sysstat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int sysstat_parse_cpu(const char *s, CpuTimes *o) {
    const char *p = (strncmp(s, "cpu ", 4) == 0) ? s : strstr(s, "\ncpu ");
    if (!p) return -1;
    if (*p == '\n') p++;
    memset(o, 0, sizeof(*o));
    int n = sscanf(p, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        (unsigned long long *)&o->user,    (unsigned long long *)&o->nice,
        (unsigned long long *)&o->system,  (unsigned long long *)&o->idle,
        (unsigned long long *)&o->iowait,  (unsigned long long *)&o->irq,
        (unsigned long long *)&o->softirq, (unsigned long long *)&o->steal);
    return (n >= 4) ? 0 : -1;
}

static uint64_t busy(const CpuTimes *c) {
    return c->user + c->nice + c->system + c->irq + c->softirq + c->steal;
}
static uint64_t total(const CpuTimes *c) { return busy(c) + c->idle + c->iowait; }

int sysstat_cpu_busy_pct(const CpuTimes *prev, const CpuTimes *cur) {
    /* /proc/stat jiffy fields are monotone kernel counters, so a healthy
     * sample has cur >= prev for both busy and total. Guard both before the
     * unsigned subtractions are used, so a non-monotone sample can't wrap db/dt
     * into a garbage quotient (which the int cast would then truncate). */
    if (total(cur) < total(prev) || busy(cur) < busy(prev)) return 0;
    uint64_t dt = total(cur) - total(prev);
    uint64_t db = busy(cur) - busy(prev);
    if (dt == 0) return 0;
    return (int)((db * 100 + dt / 2) / dt);
}

int sysstat_mem_used_pct(const char *m) {
    long tot = -1, avail = -1;
    const char *p;
    if ((p = strstr(m, "MemTotal:")))     tot   = strtol(p + 9, NULL, 10);
    if ((p = strstr(m, "MemAvailable:"))) avail = strtol(p + 13, NULL, 10);
    if (tot <= 0 || avail < 0 || avail > tot) return -1;
    return (int)(((tot - avail) * 100 + tot / 2) / tot);
}

int sysstat_thermal_c(const char *t) {
    char *end = NULL;
    long milli = strtol(t, &end, 10);
    if (end == t) return -1;
    return (int)(milli / 1000);
}

long sysstat_stream_uptime_secs(const char *up_s, const char *stat_s,
                                long clk_tck) {
    if (!up_s || !stat_s || clk_tck <= 0) return -1;

    char *end = NULL;
    double up = strtod(up_s, &end);
    if (end == up_s || up <= 0.0) return -1;

    /* /proc/<pid>/stat: field 2 (comm) is parenthesised and may itself contain
     * spaces or ')', so scan from the LAST ')'. The tokens after it begin at
     * field 3; starttime is field 22 -> the 20th token after the ')'. */
    const char *p = strrchr(stat_s, ')');
    if (!p) return -1;
    p++;

    unsigned long long starttime = 0;
    int field = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        field++;
        char *tok_end = NULL;
        unsigned long long v = strtoull(p, &tok_end, 10);
        if (field == 20) { starttime = v; break; }
        if (tok_end == p) {            /* non-numeric token (e.g. state char) */
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        } else {
            p = tok_end;
        }
    }
    if (field < 20) return -1;

    double started = (double)starttime / (double)clk_tck;
    double dur = up - started;
    if (dur < 0.0) dur = 0.0;
    return (long)(dur + 0.5);
}

long sysstat_throttle_bits(const char *s) {
    if (!s) return -1;
    const char *p = strstr(s, "0x");
    if (!p) return -1;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 16);
    if (end == p) return -1;
    return (long)v;
}
