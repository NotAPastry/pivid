/* Feeder-level frame-drop AQM -- see aqm.h and docs/aqm/README.md. */

#define _POSIX_C_SOURCE 200809L

#include "aqm.h"
#include "tx.h"
#include "tx_csv.h"
#include "ring_user.h"      /* ring_rd_ptr / ring_wr_ptr */
#include "radio_ioctl.h"    /* RADIO_RING_DATA_BYTES     */
#include <stdlib.h>
#include <string.h>
#include <time.h>           /* clock_gettime             */

int aqm_is_vcl(uint8_t nal0) {
    uint8_t nut = nal0 & 0x1F;
    return nut >= 1 && nut <= 5;
}

int aqm_is_au_start(const uint8_t *nal, size_t len) {
    if (len < 2) return 0;
    if (!aqm_is_vcl(nal[0])) return 0;
    return (nal[1] & 0x80) != 0;   /* first_mb_in_slice == 0 (ue(v)==0) */
}

AqmClass aqm_classify(const uint8_t *nal, size_t len) {
    (void)len;                     /* nal[0] suffices; caller guarantees len>=1 */
    uint8_t nal0 = nal[0];
    if ((nal0 & 0x1F) == 5)  return AQM_CLASS_IDR;
    if ((nal0 & 0x60) != 0)  return AQM_CLASS_REF;
    return AQM_CLASS_DISP;
}

AqmLevel aqm_hysteresis_step(AqmLevel cur, unsigned pct,
                             unsigned t_disp_hi, unsigned t_disp_lo,
                             unsigned t_ref_hi,  unsigned t_ref_lo) {
    switch (cur) {
    case AQM_LVL_NONE:
        if (pct >= t_ref_hi)  return AQM_LVL_REF;   /* allow a direct jump */
        if (pct >= t_disp_hi) return AQM_LVL_DISP;
        return AQM_LVL_NONE;
    case AQM_LVL_DISP:
        if (pct >= t_ref_hi)  return AQM_LVL_REF;
        if (pct <  t_disp_lo) return AQM_LVL_NONE;
        return AQM_LVL_DISP;
    case AQM_LVL_REF:
    default:
        if (pct <  t_disp_lo) return AQM_LVL_NONE;  /* collapse */
        if (pct <  t_ref_lo)  return AQM_LVL_DISP;
        return AQM_LVL_REF;
    }
}

/* [recipe][class] -> minimum level to drop. Columns: IDR, REF, DISP. */
static const AqmLevel AQM_POLICY[AQM_RCP_COUNT][3] = {
    /* gdr      */ { AQM_LVL_NEVER, AQM_LVL_DISP,  AQM_LVL_DISP },
    /* droppable*/ { AQM_LVL_NEVER, AQM_LVL_REF,   AQM_LVL_DISP },
    /* gop      */ { AQM_LVL_NEVER, AQM_LVL_DISP,  AQM_LVL_DISP },
    /* fallback */ { AQM_LVL_NEVER, AQM_LVL_DISP,  AQM_LVL_DISP },
};

AqmLevel aqm_policy_min_level(AqmRecipe r, AqmClass c) {
    if (r < 0 || r >= AQM_RCP_COUNT) r = AQM_RCP_FALLBACK;
    if (c < 0 || c > AQM_CLASS_DISP) c = AQM_CLASS_REF;
    return AQM_POLICY[r][c];
}

int aqm_should_drop(AqmRecipe r, AqmLevel cur, AqmClass c) {
    AqmLevel min = aqm_policy_min_level(r, c);
    return (min != AQM_LVL_NEVER) && ((int)cur >= (int)min);
}

#define AQM_CSV_DEFAULT "/run/pivid/txsample.csv"

AqmRecipe aqm_parse_recipe(const char *s) {
    if (!s) return AQM_RCP_FALLBACK;
    if (!strcmp(s, "gdr"))       return AQM_RCP_GDR;
    if (!strcmp(s, "droppable")) return AQM_RCP_DROPPABLE;
    if (!strcmp(s, "gop"))       return AQM_RCP_GOP;
    return AQM_RCP_FALLBACK;
}

AqmRecipe aqm_recipe_from_encoder(const char *recipe) {
    /* gdr_idr emits periodic full IDR keyframes (IDR_PERIOD), not continuous
     * intra-refresh, so its drop policy is GOP's, not plain gdr's. */
    if (recipe && !strcmp(recipe, "gdr_idr")) return AQM_RCP_GOP;
    return aqm_parse_recipe(recipe);
}

AqmSignal aqm_parse_signal(const char *s) {
    if (!s) return AQM_SIG_PEAK;
    if (!strcmp(s, "inst")) return AQM_SIG_INST;
    if (!strcmp(s, "ewma")) return AQM_SIG_EWMA;
    if (!strcmp(s, "res"))  return AQM_SIG_RES;
    return AQM_SIG_PEAK;
}

static unsigned env_pct(const char *name, unsigned def) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    long n = strtol(v, NULL, 10);
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    return (unsigned)n;
}

int aqm_start(Aqm *a, struct TX_Dev *dev, const AqmConfig *cfg) {
    memset(a, 0, sizeof(*a));
    dev->aqm = a;                         /* wire the hook even when OFF       */

    a->mode      = cfg->mode;
    a->recipe    = cfg->recipe;
    a->signal    = aqm_parse_signal(getenv("PIVID_AQM_SIGNAL"));
    a->t_disp_hi = env_pct("PIVID_AQM_T_DISP_HIGH", 40);
    a->t_disp_lo = env_pct("PIVID_AQM_T_DISP_LOW",  25);
    a->t_ref_hi  = env_pct("PIVID_AQM_T_REF_HIGH",  55);
    a->t_ref_lo  = env_pct("PIVID_AQM_T_REF_LOW",   45);
    a->level     = AQM_LVL_NONE;

    if (a->mode == AQM_OFF) return 0;     /* dormant: hooks see OFF, no CSV    */

    a->dev = dev;

    /* CSV logging is measurement-only: acquire the shared sink ONLY when the
     * sampler flag is set (same check as tx_sample.c). Enforcement does not
     * depend on it, so enforce-by-default never grows the tmpfs CSV. A failed
     * open is best-effort -- warn and keep enforcing. */
    const char *en = getenv("PIVID_TX_SAMPLE");
    int sampling = en && *en && !(en[0] == '0' && en[1] == '\0');
    const char *path = getenv("PIVID_TX_SAMPLE_CSV");
    if (!path || !*path) path = AQM_CSV_DEFAULT;
    if (sampling) {
        a->csv = txcsv_acquire(path);
        if (!a->csv)
            fprintf(stderr, "pivid-tx: AQM csv open failed (%s); "
                            "continuing without logging\n", path);
    }

    fprintf(stderr, "pivid-tx: AQM ENFORCE (recipe=%d signal=%d wm=%u/%u/%u/%u)%s%s\n",
            (int)a->recipe, (int)a->signal,
            a->t_disp_hi, a->t_disp_lo, a->t_ref_hi, a->t_ref_lo,
            a->csv ? " -> " : "", a->csv ? path : "");
    return 0;
}

void aqm_stop(Aqm *a, struct TX_Dev *dev) {
    if (a->mode != AQM_OFF && a->csv) { txcsv_release(); a->csv = NULL; }
    a->mode = AQM_OFF;
    if (dev) dev->aqm = NULL;
}

static uint64_t aqm_mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static unsigned used_to_pct(uint32_t used) {
    return (unsigned)((uint64_t)used * 100ull / RADIO_RING_DATA_BYTES);
}

int aqm_filter_nal(Aqm *a, const uint8_t *nal, size_t len) {
    if (!a || a->mode == AQM_OFF) return 1;        /* dormant: single branch  */
    if (len == 0) return 1;
    /* non-VCL (SPS/PPS/SEI/AUD): keep unconditionally, latch untouched. This is
     * what protects parameter sets -- they exit before the dropping_au gate, so
     * a congestion-dropped AU can never strand SPS/PPS ahead of its IDR. */
    if (!aqm_is_vcl(nal[0])) return 1;

    if (aqm_is_au_start(nal, len)) {
        /* --- sample all candidate signals (rd before wr; no underflow) --- */
        uint32_t rd   = __atomic_load_n(ring_rd_ptr(a->dev->tx_ring), __ATOMIC_ACQUIRE);
        uint32_t wr   = __atomic_load_n(ring_wr_ptr(a->dev->tx_ring), __ATOMIC_RELAXED);
        uint32_t inst = wr - rd;
        /* Grab + reset the per-frame peak in one atomic op: the exchange reads
         * the value (used for THIS decision) and zeroes it for the next frame,
         * so the reset cannot precede the read. */
        uint32_t peak = __atomic_exchange_n(&a->peak_used, 0, __ATOMIC_RELAXED);
        if (!a->ewma_init) { a->ewma_used = inst << 8; a->ewma_init = 1; }
        else a->ewma_used = (uint32_t)((int32_t)a->ewma_used +
                            (((int32_t)(inst << 8) - (int32_t)a->ewma_used) >> 3));
        uint32_t ewma = a->ewma_used >> 8;
        unsigned est_res_ms = (unsigned)((uint64_t)inst * 8ull / AQM_DRAIN_KBPS);

        /* --- select the comparator input (res == inst by construction) --- */
        uint32_t sig_used;
        switch (a->signal) {
        case AQM_SIG_PEAK: sig_used = peak; break;
        case AQM_SIG_EWMA: sig_used = ewma; break;
        case AQM_SIG_INST:
        case AQM_SIG_RES:
        default:           sig_used = inst; break;
        }
        unsigned pct = used_to_pct(sig_used);

        /* --- gate + classify + decide --- */
        a->level = aqm_hysteresis_step(a->level, pct, a->t_disp_hi,
                                       a->t_disp_lo, a->t_ref_hi, a->t_ref_lo);
        AqmClass cls = aqm_classify(nal, len);
        int drop = aqm_should_drop(a->recipe, a->level, cls);

        a->au_seen++;
        if (drop) a->au_dropped[cls]++; else a->au_kept[cls]++;

        /* --- log one row (single fprintf: stdio locks the shared stream) --- */
        if (a->csv) {
            fprintf(a->csv,
                "AQM,%llu,%d,%d,%d,%s,%d,%u,%u,%u,%u,%u\n",
                (unsigned long long)aqm_mono_ns(),
                (int)cls, (int)a->recipe, (int)a->level,
                drop ? "DROP" : "KEEP",
                (int)a->signal, used_to_pct(sig_used),
                used_to_pct(inst), used_to_pct(peak), used_to_pct(ewma),
                est_res_ms);
            fflush(a->csv);
        }

        /* Latch the decision for the whole AU (mode is ENFORCE here -- OFF
         * returned early above). */
        a->dropping_au = drop;
    }

    if (a->dropping_au) { a->nal_suppressed++; return 0; }   /* SUPPRESS */
    return 1;                                                /* KEEP */
}
