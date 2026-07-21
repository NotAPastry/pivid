/* Pure rpicam-vid argv builder + recipe->flags table. pivid-tx spawns
 * rpicam-vid itself (see camera_spawn.c), so this is the single place the
 * encoder command line is built. Sibling to common/recipe_defaults.c
 * (per-recipe FORM defaults); this file is the per-recipe ENCODER FLAG SET.
 *
 * CONSTANT FLAGS: -t 0 --inline --codec h264
 * --level 4.2 -o -.  --inline forces SPS/PPS in every IDR so the RX recovers
 * without an IDR-request channel.
 *
 * PROFILE is recipe-dependent, NOT constant. The VPU firmware's CBR rate
 * controller only works under Baseline profile: Main+CBR and High+CBR are
 * rejected at STREAMON ("Failed enabling i/p port, ret -3" = MMAL_EINVAL),
 * HW-verified; see docs/DECISIONS.md "CBR recipes need Baseline profile".
 * So the CBR recipes (gdr/gdr_idr/gop) MUST use --profile baseline; only the VBR
 * droppable recipe uses --profile main for CABAC (~10-15% better compression, no
 * B-frames so latency is unchanged). A global --profile main
 * silently broke all CBR recipes once, while leaving VBR droppable working --
 * which is why this is per-recipe and not a constant.
 *
 * DO NOT ADD --hflip / --vflip / --rotation / --denoise off (or any flag that
 * forces software pixel processing). libcamera's zero-copy DMA-buf path requires
 * sensor -> ISP -> encoder with no ARM-side CPU step; any of those flags pulls
 * frames back to CPU memory and costs 30-50 ms/frame on a Cortex-A53.
 * --width/--height are SAFE (ISP hardware scaling, not CPU). */

#define _POSIX_C_SOURCE 200809L
#include "camera_argv.h"

#include <stdio.h>
#include <string.h>

/* Append a string literal as the next argv slot. Returns 0, or -1 on overflow. */
static int push_lit(CameraArgv *o, const char *lit) {
    if (o->argc >= CAMERA_ARGV_MAX - 1) return -1;   /* keep room for NULL */
    o->argv[o->argc++] = (char *)lit;
    return 0;
}

/* Format an int into the next slot's store[] and append it. */
static int push_num(CameraArgv *o, int v) {
    if (o->argc >= CAMERA_ARGV_MAX - 1) return -1;
    char *slot = o->store[o->argc];
    snprintf(slot, CAMERA_ARGV_VALBYTES, "%d", v);
    o->argv[o->argc++] = slot;
    return 0;
}

#define LIT(x)        do { if (push_lit(o, (x)))             return -1; } while (0)
#define PAIR_LIT(f,v) do { if (push_lit(o,(f))||push_lit(o,(v))) return -1; } while (0)
#define PAIR_NUM(f,n) do { if (push_lit(o,(f))||push_num(o,(n))) return -1; } while (0)

int camera_build_argv(const Settings *s, const char *bin, CameraArgv *out) {
    if (!s || !bin || !out) return -1;
    CameraArgv *o = out;
    memset(o, 0, sizeof *o);

    const int is_drop    = strcmp(s->recipe, "droppable") == 0;
    const int is_gdr     = strcmp(s->recipe, "gdr")       == 0;
    const int is_gdr_idr = strcmp(s->recipe, "gdr_idr")   == 0;
    const int is_gop     = strcmp(s->recipe, "gop")       == 0;
    if (!is_drop && !is_gdr && !is_gdr_idr && !is_gop) return -1;

    /* Trailing --intra value, recipe-adjusted:
     *   gdr     -> 0           (refresh driven by --intra-refresh, no IDR cadence)
     *   gdr_idr -> idr_period  (periodic IDR so the RX HW decoder can lock)
     *   else    -> config intra. */
    const int intra_val = is_gdr ? 0 : (is_gdr_idr ? s->idr_period : s->intra);

    if (push_lit(o, bin)) return -1;            /* argv[0] */
    PAIR_LIT("-t", "0");
    LIT("--inline");
    PAIR_NUM("--width",     s->width);
    PAIR_NUM("--height",    s->height);
    PAIR_NUM("--framerate", s->fps);
    PAIR_NUM("--bitrate",   s->bitrate);
    PAIR_NUM("--intra",     intra_val);

    /* Per-recipe flag set.
     *
     * Only `gdr` enables --intra-refresh (the cyclic-refresh window = config
     * intra, never intra_val). gdr_idr does NOT: on this firmware cyclic refresh
     * and periodic IDRs are mutually exclusive -- refresh REPLACES the IDRs that
     * --intra (idr_period) requests, so a refresh+IDR recipe emits exactly one
     * IDR (the startup one) and a late-joining decoder can never re-acquire
     * within seconds. Measured 2026-06-24 (cold-join PSNR heal harness): with
     * refresh, gdr_idr healed only after 5-12 s (sometimes not at all); WITHOUT
     * refresh its periodic IDRs give bit-exact re-acquire within <= idr_period
     * (<=2 s @60f). So gdr_idr drops refresh and rides its IDR cadence -- which
     * is exactly what its name/purpose ("periodic IDR so the HW decoder can
     * lock") always claimed. `gdr` keeps refresh as the pure no-IDR-burst
     * resilience recipe (late-join is inherently slow there, by firmware
     * constraint -- the player's opt-in +showall only shows progressive video).
     * Trial finding: the firmware does refresh XOR IDR, and the refresh period
     * is not tunable. See docs/DECISIONS.md "GDR cannot fast late-join". */
    if (is_drop) {
        PAIR_LIT("--bitrate-mode", "vbr");
        LIT("--droppable-pframes");
        PAIR_NUM("--droppable-len", s->droppable_len);
        PAIR_NUM("--slice-mb",      s->slice_mb);
        /* IMP-1: per-frame trailer NAL so the TX Annex-B parser releases each
         * frame's tail slice immediately (~1-frame glass-to-glass reclaim).
         * Live latency recipe only; inert filler NAL, no wire-format change. */
        LIT("--frame-trailer");
    } else if (is_gdr) {
        PAIR_LIT("--bitrate-mode", "cbr");
        PAIR_NUM("--intra-refresh", s->intra);
        PAIR_NUM("--slice-mb",      s->slice_mb);
    } else { /* gdr_idr or gop: CBR, periodic IDR via --intra (intra_val), no refresh */
        PAIR_LIT("--bitrate-mode", "cbr");
        PAIR_NUM("--slice-mb",      s->slice_mb);
    }

    /* CBR recipes require Baseline (firmware Main/High+CBR incompatibility,
     * see file header); only the VBR droppable recipe can use Main (CABAC). */
    PAIR_LIT("--codec",   "h264");
    PAIR_LIT("--profile", is_drop ? "main" : "baseline");
    PAIR_LIT("--level",   "4.2");
    PAIR_LIT("-o", "-");

    o->argv[o->argc] = NULL;   /* execvp() terminator */
    return 0;
}
