#ifndef TX_AQM_H
#define TX_AQM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct TX_Dev;   /* forward decl: aqm_start wires dev->aqm + reads the ring */

/* Operating mode. OFF is the build default; the config layer ships ENFORCE. */
typedef enum { AQM_OFF = 0, AQM_ENFORCE } AqmMode;

/* Congestion level from the hysteresis gate (also used as a policy threshold;
 * AQM_LVL_NEVER is the sentinel "never drop this class"). */
typedef enum { AQM_LVL_NONE = 0, AQM_LVL_DISP = 1, AQM_LVL_REF = 2,
               AQM_LVL_NEVER = 3 } AqmLevel;

/* Frame class from the AU-start NAL. Order is the policy-table column index. */
typedef enum { AQM_CLASS_IDR = 0, AQM_CLASS_REF = 1, AQM_CLASS_DISP = 2 } AqmClass;

/* Encoder recipe -> policy row. Order is the policy-table row index. */
typedef enum { AQM_RCP_GDR = 0, AQM_RCP_DROPPABLE = 1, AQM_RCP_GOP = 2,
               AQM_RCP_FALLBACK = 3, AQM_RCP_COUNT = 4 } AqmRecipe;

/* Config-driven AQM inputs (mode + recipe). The hysteresis tuning (signal,
 * watermarks) stays hidden env (see aqm_start). */
typedef struct {
    AqmMode   mode;
    AqmRecipe recipe;
} AqmConfig;

/* Gate comparator input selector (PIVID_AQM_SIGNAL). */
typedef enum { AQM_SIG_PEAK = 0, AQM_SIG_INST, AQM_SIG_EWMA, AQM_SIG_RES } AqmSignal;

/* --- Pure cores (no I/O, no globals; host-testable) --- */

/* VCL NAL = coded slice (nal_unit_type 1..5). */
int      aqm_is_vcl(uint8_t nal0);

/* AU start = first VCL NAL of a new picture: first_mb_in_slice==0, which for
 * ue(v)==0 is simply the top bit of the first RBSP byte (nal[1]) being set. */
int      aqm_is_au_start(const uint8_t *nal, size_t len);

/* Frame class from the NAL header byte: IDR (nut==5) > REF (ref_idc!=0) > DISP. */
AqmClass aqm_classify(const uint8_t *nal, size_t len);

/* 3-state hysteresis on a percent-of-ring input. Returns the new level given
 * the current level and the four watermarks (percent). Dual-threshold:
 *   NONE       -> DISP at >= t_disp_hi
 *   DISP       -> NONE at <  t_disp_lo,  -> REF at >= t_ref_hi
 *   REF        -> DISP at <  t_ref_lo,   -> NONE at <  t_disp_lo */
AqmLevel aqm_hysteresis_step(AqmLevel cur, unsigned pct,
                             unsigned t_disp_hi, unsigned t_disp_lo,
                             unsigned t_ref_hi,  unsigned t_ref_lo);

/* Minimum congestion level at which a (recipe,class) frame is dropped, or
 * AQM_LVL_NEVER. Pure table lookup. */
AqmLevel aqm_policy_min_level(AqmRecipe r, AqmClass c);

/* drop = (min != NEVER) && (cur >= min). */
int      aqm_should_drop(AqmRecipe r, AqmLevel cur, AqmClass c);

/* Drain-rate constant for the est_res_ms signal (kbps). */
#define AQM_DRAIN_KBPS 1244u

typedef struct Aqm {
    AqmMode   mode;        /* OFF => all hooks no-op                          */
    AqmSignal signal;      /* active gate comparator input                    */
    AqmRecipe recipe;      /* policy row                                      */
    unsigned  t_disp_hi, t_disp_lo, t_ref_hi, t_ref_lo;   /* percent-of-ring  */

    struct TX_Dev *dev;    /* for ring rd/wr reads; NULL when OFF             */
    FILE     *csv;         /* shared sink FILE*; NULL when OFF                 */

    /* Hysteresis + signal state. Encoder-thread-only EXCEPT peak_used, which
     * is also written by aqm_on_push (under tx_push's producer_lock) and
     * atomically exchanged-to-0 by the encoder thread at each AU boundary. */
    AqmLevel  level;
    uint32_t  peak_used;   /* atomic: __atomic_* only                         */
    uint32_t  ewma_used;   /* fixed-point <<8; encoder thread only            */
    int       ewma_init;
    int       dropping_au; /* current-AU suppression latch (ENFORCE)          */

    /* Counters (encoder thread; read by the heartbeat). */
    uint64_t  au_seen;
    uint64_t  au_dropped[3];   /* indexed by AqmClass                          */
    uint64_t  au_kept[3];
    uint64_t  nal_suppressed;  /* VCL NALs actually withheld from fec_tx       */
} Aqm;

/* Mode + recipe come from cfg (filled by main from Settings); the hysteresis
 * tuning (signal, watermarks, CSV) still reads hidden env (PIVID_AQM_SIGNAL,
 * AQM_T_*, PIVID_TX_SAMPLE[_CSV]). OFF: zero-init, wire dev->aqm (hooks
 * no-op), return 0. ENFORCE: acquire the shared CSV, wire dev->aqm +
 * capture dev (for ring rd/wr reads via dev->tx_ring), return 0; return -1 only
 * if an explicitly-requested mode failed to open the CSV. */
int  aqm_start(Aqm *a, struct TX_Dev *dev, const AqmConfig *cfg);

/* Release the CSV ref, clear dev->aqm. Idempotent. */
void aqm_stop(Aqm *a, struct TX_Dev *dev);

/* Parse helpers (exposed for unit tests). */
AqmRecipe aqm_parse_recipe(const char *s);
AqmSignal aqm_parse_signal(const char *s);

/* Map an encoder recipe string (ENCODER_RECIPE) to the AQM policy row.
 * Adds the gdr_idr -> GOP policy mapping on top of aqm_parse_recipe; unknown
 * -> AQM_RCP_FALLBACK. main.c uses this so the AQM recipe always tracks the
 * encoder recipe.
 * gdr_idr emits periodic full IDR keyframes (IDR_PERIOD) rather than continuous
 * intra-refresh, so its frame-class drop policy matches GOP's (IDR anchor /
 * never-drop, P ref/disposable) -- unlike plain gdr, which has no IDR cadence. */
AqmRecipe aqm_recipe_from_encoder(const char *recipe);

/* Producer hook -- called from tx_push under producer_lock, right after
 * ring_user_push succeeds, with the just-published wr and the current rd.
 * Tracks the per-frame peak occupancy via an atomic max. Inline so the dormant
 * path is a single branch. No-op until mode != OFF. */
static inline void aqm_on_push(struct Aqm *a, uint32_t wr_after, uint32_t rd_now) {
    if (!a || __atomic_load_n(&a->mode, __ATOMIC_RELAXED) == AQM_OFF) return;
    uint32_t used = wr_after - rd_now;
    uint32_t cur  = __atomic_load_n(&a->peak_used, __ATOMIC_RELAXED);
    while (used > cur &&
           !__atomic_compare_exchange_n(&a->peak_used, &cur, used, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        /* cur reloaded by the builtin on failure; retry */
    }
}

/* Per-NAL filter -- called from the encoder thread in emit_nal_to_chunker
 * BEFORE fec_tx_nal. Returns 1 = KEEP (push to FEC), 0 = SUPPRESS (drop).
 * OFF: single-branch KEEP. Non-VCL: always KEEP (latch untouched). At an AU
 * boundary: sample the gate, classify, decide, log, set the latch. */
int aqm_filter_nal(Aqm *a, const uint8_t *nal, size_t len);

#endif /* TX_AQM_H */
