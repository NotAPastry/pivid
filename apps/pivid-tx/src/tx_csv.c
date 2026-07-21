#define _POSIX_C_SOURCE 200809L

#include "tx_csv.h"

/* Shared header: the sampler's OCC/RES/DROP columns. AQM rows append extra
 * trailing columns; CSV readers key on the `kind` column and tolerate the
 * variable tail (documented in docs/aqm/README.md). */
#define TXCSV_HEADER "kind,mono_ns,used,occ_pct,temp_mC,wake_skew_ns,res_seq,res_ns\n"

static FILE *g_fp;
static int   g_refs;

FILE *txcsv_acquire(const char *path) {
    if (g_refs > 0) { g_refs++; return g_fp; }
    g_fp = fopen(path, "w");
    if (!g_fp) return NULL;
    fputs(TXCSV_HEADER, g_fp);
    fflush(g_fp);
    g_refs = 1;
    return g_fp;
}

void txcsv_release(void) {
    if (g_refs <= 0) return;
    if (--g_refs == 0) {
        if (g_fp) { fflush(g_fp); fclose(g_fp); g_fp = NULL; }
    }
}
