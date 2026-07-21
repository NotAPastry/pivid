/* Pure camera-argv builder: assert the exact rpicam-vid flag set per recipe,
 * including the gdr_idr --intra (idr_period) vs --intra-refresh (intra) split.
 * No process is spawned. */
#include "camera_argv.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;

/* Join argv (skipping argv[0]) into one space-separated string for comparison. */
static void join_args(const CameraArgv *c, char *buf, size_t cap) {
    size_t o = 0;
    for (int i = 1; i < c->argc; i++) {
        int w = snprintf(buf + o, cap - o, "%s%s", i == 1 ? "" : " ", c->argv[i]);
        assert(w > 0 && (size_t)w < cap - o);
        o += (size_t)w;
    }
    buf[o] = '\0';
}

static void check(const char *recipe, const char *expect) {
    Settings s;
    memset(&s, 0, sizeof s);
    snprintf(s.recipe, sizeof s.recipe, "%s", recipe);
    s.width = 640; s.height = 480; s.fps = 30; s.bitrate = 650000;
    s.intra = 30; s.slice_mb = 120; s.droppable_len = 8; s.idr_period = 60;

    CameraArgv c;
    int rc = camera_build_argv(&s, "rpicam-vid", &c);
    if (rc != 0) { printf("FAIL %s: build returned %d\n", recipe, rc); fails++; return; }
    if (strcmp(c.argv[0], "rpicam-vid") != 0) { printf("FAIL %s: argv[0]=%s\n", recipe, c.argv[0]); fails++; }
    if (c.argv[c.argc] != NULL) { printf("FAIL %s: argv not NULL-terminated\n", recipe); fails++; }

    char got[1024];
    join_args(&c, got, sizeof got);
    if (strcmp(got, expect) != 0) {
        printf("FAIL %s:\n  expect: %s\n  got:    %s\n", recipe, expect, got);
        fails++;
    } else {
        printf("OK %s\n", recipe);
    }
}

int main(void) {
    const char *PREFIX = "-t 0 --inline --width 640 --height 480 "
                         "--framerate 30 --bitrate 650000 --intra ";
    /* Profile is recipe-dependent: VBR droppable uses Main (CABAC); the CBR
     * recipes require Baseline (firmware Main/High+CBR incompatibility). */
    const char *SUFFIX_MAIN = " --codec h264 --profile main --level 4.2 -o -";
    const char *SUFFIX_BASE = " --codec h264 --profile baseline --level 4.2 -o -";
    char exp[1024];

    /* droppable: --intra=config(30), VBR + droppable-pframes/len + slices +
     * frame-trailer (IMP-1 tail-slice release), Main */
    snprintf(exp, sizeof exp, "%s30 --bitrate-mode vbr --droppable-pframes "
             "--droppable-len 8 --slice-mb 120 --frame-trailer%s", PREFIX, SUFFIX_MAIN);
    check("droppable", exp);

    /* gdr: --intra=0, CBR + intra-refresh(30) + slices, Baseline */
    snprintf(exp, sizeof exp, "%s0 --bitrate-mode cbr --intra-refresh 30 "
             "--slice-mb 120%s", PREFIX, SUFFIX_BASE);
    check("gdr", exp);

    /* gdr_idr: --intra=idr_period(60), CBR + slices, NO intra-refresh, Baseline.
     * Refresh and periodic IDR are mutually exclusive on this firmware (refresh
     * replaces the IDRs); gdr_idr drops refresh so its periodic IDRs survive and
     * a late joiner re-acquires within <=idr_period. See camera_argv.c. */
    snprintf(exp, sizeof exp, "%s60 --bitrate-mode cbr --slice-mb 120%s",
             PREFIX, SUFFIX_BASE);
    check("gdr_idr", exp);

    /* gop: --intra=config(30), CBR + slices (no intra-refresh, no droppable), Baseline */
    snprintf(exp, sizeof exp, "%s30 --bitrate-mode cbr --slice-mb 120%s",
             PREFIX, SUFFIX_BASE);
    check("gop", exp);

    /* unknown recipe -> -1 */
    {
        Settings s; memset(&s, 0, sizeof s);
        snprintf(s.recipe, sizeof s.recipe, "%s", "weird");
        CameraArgv c;
        if (camera_build_argv(&s, "rpicam-vid", &c) != -1) {
            printf("FAIL unknown: expected -1\n"); fails++;
        } else { printf("OK unknown -> -1\n"); }
    }

    /* NULL args -> -1 (no crash) */
    {
        CameraArgv c;
        Settings s; memset(&s, 0, sizeof s);
        snprintf(s.recipe, sizeof s.recipe, "%s", "gop");
        if (camera_build_argv(NULL, "x", &c) != -1 ||
            camera_build_argv(&s, NULL, &c) != -1 ||
            camera_build_argv(&s, "x", NULL) != -1) {
            printf("FAIL NULL-guard\n"); fails++;
        } else { printf("OK NULL-guard -> -1\n"); }
    }

    if (fails) { printf("%d FAILURE(S)\n", fails); return 1; }
    printf("all camera_argv tests passed\n");
    return 0;
}
