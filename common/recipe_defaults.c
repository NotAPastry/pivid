#define _POSIX_C_SOURCE 200809L
#include "recipe_defaults.h"
#include <stdio.h>
#include <string.h>

/* Per-recipe form defaults for the config-agent dashboard.
 *
 * CONSTRAINT: FEC_M_REF >= 1 for every recipe ("FEC always on").
 *
 * Provenance:
 *  - `droppable` is MEASURED + RF-CONFIRMED. Offline 7668-run joint sweep:
 *    m_anchor 7-9, m_ref=2 (costs ~1% vs the m_ref=0 iid optimum but wins ~18%
 *    under short bursts), k=16. droppable's ref-P is only ~24% of P frames
 *    (rest disposable, FEC-off) so m_ref is cheap. On-air Phase E A/B 2026-06-22:
 *    9,2,16 beats prior production 5,0 at every loss band (sink_err=0 up to 27%
 *    loss). The win is m_anchor 5->9; m_ref=2 showed NO measurable benefit on the
 *    ~iid bench channel -- unverified-but-harmless insurance, NOT the short-burst
 *    regime where it should help. RE-SWEEP 2026-06-27 put m_ref 0/1/2 through SYNTHETIC GE
 *    bursts to length 34 (anchored to a real on-air measurement of ~5-34-packet
 *    bursts): PSNR stayed flat (16.06-16.10 dB) across m_ref even there, so m_ref=2
 *    is still unjustified by delivered quality and costs ~24% more parity than
 *    m_ref=0. m_anchor=9 RE-VALIDATED (monotonic: higher is better). Briefly
 *    DROPPED m_ref 2->1 (2026-06-29) because worst-frame PSNR was flat across
 *    m_ref in the offline study. RESTORED m_ref 1->2 (2026-07-03, live RCA --
 *    see docs/MEASUREMENTS.md "FEC m_ref"): the offline
 *    sweeps were BLIND to the real failure mode. On the live link the Mac player
 *    showed intermittent "blocky lines"; an RX fec_unrec root-cause split
 *    (unrec_short/present/deficit -- apps/pivid-rx/src/fec_rx.c) proved 100% of
 *    unrecoverable ref-P blocks were SHORT (have < k) with mean deficit ~1: a
 *    bursty RF erasure lost m+1 symbols and m_ref=1 tolerates only 1, so RS decode
 *    failed and the H.264 decoder concealed the slice. m_ref 1->2 drove fec_unrec
 *    ~12/min -> 0 for +~4% wire (AQM never fired). The ~1% parity "reclaim" at
 *    m_ref=1 was a false economy paid for in visible artifacts. TX-only, SIGHUP-
 *    reloadable (RX reads parity off the wire). Floor is m_ref>=1, "FEC always on".
 *  - FEC INTERLEAVE is DORMANT at the sliced production config (re-sweep 2026-06-27):
 *    fec_tx only column-interleaves equal-(k,n) held blocks, but sliced video gives
 *    one small block per NAL with varying k, so emit_pair_interleaved never fires
 *    (ilv on/off produce byte-identical ES). Harmless to leave on; it only engages
 *    for single-slice large-NAL profiles. Making it effective with slicing needs
 *    cross-NAL/class block pairing.
 *  - `gop` / `gdr` / `gdr_idr` are PROVISIONAL, structurally reasoned, pending
 *    Phase E. All their P frames are reference, so m_ref is ~4x the airtime of
 *    droppable's -> keep low (1-2), except gdr which has no IDR safety net at
 *    all, so its ref-P parity is intentionally the heaviest (m_ref=4, still >=1).
 *  - slice_mb=120 VALIDATED clean-link 2026-06-26 (see docs/MEASUREMENTS.md
 *    "slice_mb"): on the production VPU it is the worst row
 *    of the {0,120,160,200,400} sweep -- +13% airtime (per-packet framing
 *    overhead; wire bytes are flat across the sweep) and -0.93 dB PSNR vs
 *    single-slice at a held 650 kbps VBR. KEPT: the link is loss-limited and 120
 *    is the fecsim sweep-best UNDER LOSS, so the modest clean-link cost buys that
 *    resilience. Follow-up DONE -- re-sweep 2026-06-27 swept slice_mb {0,120,160,
 *    200} under GE bursts: slice_mb=120 has the BEST PSNR (14.95 dB) AND best NAL
 *    delivery (0.882), single-slice is worst (lost pkt kills a whole-frame NAL),
 *    and 200 does NOT close the gap. slice_mb=120 RE-VALIDATED under loss; KEPT.
 *    (dlen=8 also decisively beats dlen=4 under burst: PSNR 14.99 vs 14.79.)
 *
 * Every recipe lists every recipe-managed key; the `shown` flag (not absence)
 * controls dashboard visibility, and a hidden field's value is still applied.
 */
const RecipeDefault recipe_defaults[] = {
    /* droppable: VBR finite-GOP + droppable-P + slices. SPS reinject off+hidden
     * (--inline carries SPS in every IDR); no periodic IDR knob. Values == the
     * factory defaults (settings_defaults), per the Reset invariant. */
    { "droppable", "BITRATE",         "850000", 1 },
    { "droppable", "SLICE_MB",        "120",    1 },
    { "droppable", "INTRA",           "30",     1 },
    { "droppable", "DROPPABLE_LEN",   "8",      1 },
    { "droppable", "IDR_PERIOD",      "60",     0 },
    { "droppable", "FEC_M_ANCHOR",    "9",      1 },
    { "droppable", "FEC_M_REF",       "2",      1 },
    { "droppable", "FEC_K",           "16",     1 },
    { "droppable", "SPS_REINJECT_MS", "0",      0 },

    /* gop: standard GOP fallback, CBR; SPS reinject on. */
    { "gop", "BITRATE",         "850000", 1 },
    { "gop", "SLICE_MB",        "120",    1 },
    { "gop", "INTRA",           "30",     1 },
    { "gop", "DROPPABLE_LEN",   "8",      0 },
    { "gop", "IDR_PERIOD",      "60",     0 },
    { "gop", "FEC_M_ANCHOR",    "5",      1 },
    { "gop", "FEC_M_REF",       "2",      1 },
    { "gop", "FEC_K",           "16",     1 },
    { "gop", "SPS_REINJECT_MS", "1000",   1 },

    /* gdr: cyclic intra-refresh, CBR, no IDR -> heaviest ref-P parity. */
    { "gdr", "BITRATE",         "850000", 1 },
    { "gdr", "SLICE_MB",        "120",    1 },
    { "gdr", "INTRA",           "30",     1 },
    { "gdr", "DROPPABLE_LEN",   "8",      0 },
    { "gdr", "IDR_PERIOD",      "60",     0 },
    { "gdr", "FEC_M_ANCHOR",    "2",      1 },
    { "gdr", "FEC_M_REF",       "4",      1 },
    { "gdr", "FEC_K",           "16",     1 },
    { "gdr", "SPS_REINJECT_MS", "1000",   1 },

    /* gdr_idr: periodic IDR (no refresh -- firmware makes refresh+IDR mutually
     * exclusive), HW-decode safe, late-join re-acquires within <=idr_period.
     * INTRA is HIDDEN here: it is the cyclic-refresh window, which gdr_idr
     * no longer uses (camera_argv reads idr_period for --intra, never s->intra);
     * IDR_PERIOD below is the meaningful cadence knob. */
    { "gdr_idr", "BITRATE",         "850000", 1 },
    { "gdr_idr", "SLICE_MB",        "120",    1 },
    { "gdr_idr", "INTRA",           "30",     0 },
    { "gdr_idr", "DROPPABLE_LEN",   "8",      0 },
    { "gdr_idr", "IDR_PERIOD",      "60",     1 },
    { "gdr_idr", "FEC_M_ANCHOR",    "5",      1 },
    { "gdr_idr", "FEC_M_REF",       "2",      1 },
    { "gdr_idr", "FEC_K",           "16",     1 },
    { "gdr_idr", "SPS_REINJECT_MS", "1000",   1 },
};
const size_t recipe_defaults_len = sizeof recipe_defaults / sizeof recipe_defaults[0];

int recipe_defaults_json_build(char *buf, size_t cap) {
    size_t o = 0;
    #define APP(...) do { int _w = snprintf(buf + o, cap - o, __VA_ARGS__); \
        if (_w < 0 || (size_t)_w >= cap - o) { return -1; } o += (size_t)_w; } while (0)
    APP("{");
    /* group consecutive rows by recipe; the table is laid out recipe-grouped */
    const char *cur = NULL;
    int first_recipe = 1;
    for (size_t i = 0; i < recipe_defaults_len; i++) {
        const RecipeDefault *d = &recipe_defaults[i];
        if (cur == NULL || strcmp(cur, d->recipe) != 0) {
            if (cur != NULL) APP("}");            /* close previous recipe obj */
            APP("%s\"%s\":{", first_recipe ? "" : ",", d->recipe);
            first_recipe = 0;
            cur = d->recipe;
            /* first member of this object: no leading comma */
            APP("\"%s\":{\"v\":\"%s\",\"show\":%s}",
                d->key, d->value, d->shown ? "true" : "false");
        } else {
            APP(",\"%s\":{\"v\":\"%s\",\"show\":%s}",
                d->key, d->value, d->shown ? "true" : "false");
        }
    }
    if (cur != NULL) APP("}");                     /* close last recipe obj */
    APP("}");
    #undef APP
    return (int)o;
}
