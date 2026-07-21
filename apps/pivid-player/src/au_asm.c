/* apps/pivid-player/src/au_asm.c -- see au_asm.h. */
#include "au_asm.h"
#include <string.h>

void au_asm_init(AuAsm *a, AuAsm_EmitCb emit, void *user) {
    memset(a, 0, sizeof *a);
    a->emit = emit;
    a->user = user;
}

static void emit_and_reset(AuAsm *a) {
    if (!a->has_vcl) return;   /* params-only buffer: keep it for the next AU */
    if (a->len > 0) {
        a->emit(a->buf, a->len, a->user);
        a->aus_emitted++;
    }
    a->len = 0;
    a->has_vcl = 0;
}

/* Offset of the NAL header byte past a 3/4-byte Annex-B start code, or 0. */
static size_t hdr_off(const uint8_t *nal, size_t len) {
    if (len >= 4 && nal[0]==0 && nal[1]==0 && nal[2]==0 && nal[3]==1) return 4;
    if (len >= 3 && nal[0]==0 && nal[1]==0 && nal[2]==1)              return 3;
    return 0;   /* no start code: treat as non-VCL/opaque (defensive) */
}
static int nal_is_vcl(const uint8_t *nal, size_t len) {
    size_t o = hdr_off(nal, len);
    if (o == 0 || len - o < 1) return 0;
    uint8_t nut = nal[o] & 0x1F;
    return nut >= 1 && nut <= 5;
}
static int nal_is_au_start(const uint8_t *nal, size_t len) {
    size_t o = hdr_off(nal, len);
    if (o == 0 || len - o < 2) return 0;
    if ((nal[o] & 0x1F) < 1 || (nal[o] & 0x1F) > 5) return 0;  /* not VCL */
    return (nal[o+1] & 0x80) != 0;                             /* first_mb==0 */
}
/* Filler-data NAL (type 12): the TX appends one after each frame's last
 * slice (IMP-1 frame trailer) -- it is the frame's end-of-AU marker. */
static int nal_is_filler(const uint8_t *nal, size_t len) {
    size_t o = hdr_off(nal, len);
    if (o == 0 || len - o < 1) return 0;
    return (nal[o] & 0x1F) == 12;
}

void au_asm_feed(AuAsm *a, const uint8_t *nal, size_t len) {
    a->nals_in++;
    if (len == 0) return;
    int au_start = nal_is_au_start(nal, len);
    int is_vcl   = nal_is_vcl(nal, len);
    /* A flush may present a PARTIAL frame (only some slices buffered when the
     * wire idled). The frame's remaining slices then arrive late as VCL
     * non-au-start NALs -- an orphaned tail. Drop them like an over-cap AU.
     * But a non-VCL NAL (SPS/PPS/SEI) or a VCL au-start after a flush is a
     * genuine new AU and MUST be kept (SPS/PPS precede the next IDR). */
    if (a->post_flush) {
        a->post_flush = 0;
        if (is_vcl && !au_start) a->dropping = 1;
    }
    /* While dropping the tail of a dropped/partial AU, swallow only its VCL
     * continuation slices; the first non-VCL NAL or au-start ends the drop
     * (so a following AU's SPS/PPS/au-start slice is preserved, not eaten). */
    if (a->dropping) {
        if (is_vcl && !au_start) return;
        a->dropping = 0;
    }
    /* A new AU begins on an au-start VCL NAL, but only if the current buffer
     * already holds a VCL slice. Leading non-VCL (SPS/PPS/SEI) before the
     * first VCL of an AU accumulate into that same (new) AU. */
    if (au_start && a->has_vcl)
        emit_and_reset(a);
    if (a->len + len > AU_ASM_CAP) {   /* overflow: drop the whole AU */
        a->overflow_drops++;
        a->len = 0; a->has_vcl = 0;
        a->dropping = 1;               /* swallow the rest of this AU */
        return;
    }
    memcpy(a->buf + a->len, nal, len);
    a->len += len;
    if (is_vcl) a->has_vcl = 1;
    /* Frame trailer: the AU is complete NOW -- emit without waiting for the
     * next frame's au-start (~33 ms) and leave nothing for a mid-frame
     * idle-flush to truncate. A stray filler with no VCL buffered is kept
     * (rides harmlessly ahead of the next AU, like leading params). */
    if (a->has_vcl && nal_is_filler(nal, len))
        emit_and_reset(a);
}

void au_asm_flush(AuAsm *a) {
    /* A VCL-bearing flush may be presenting a partial frame; arm the guard so
     * the frame's late tail slices are recognised and dropped, not re-buffered
     * headless. (A boundary-split emit in au_asm_feed does NOT arm it -- an
     * au-start there means the previous frame was already complete.) */
    int will_emit = (a->has_vcl && a->len > 0);
    emit_and_reset(a);
    if (will_emit) a->post_flush = 1;
}
