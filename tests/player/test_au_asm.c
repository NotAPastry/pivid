#include "au_asm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* --- capture harness: record emitted AUs --------------------------------- */
#define MAX_EMIT 64
static struct { uint8_t buf[AU_ASM_CAP]; size_t len; } g_emit[MAX_EMIT];
static int g_emit_n;
static void cap_reset(void) { g_emit_n = 0; }
static void cap_emit(const uint8_t *au, size_t len, void *user) {
    (void)user;
    assert(g_emit_n < MAX_EMIT);
    assert(len <= AU_ASM_CAP);
    memcpy(g_emit[g_emit_n].buf, au, len);
    g_emit[g_emit_n].len = len;
    g_emit_n++;
}

/* Build a start-code-prefixed NAL: 00 00 00 01 <hdr> <payload...> into out. */
static size_t mk_nal(uint8_t *out, uint8_t hdr, const uint8_t *pay, size_t paylen) {
    out[0]=0; out[1]=0; out[2]=0; out[3]=1; out[4]=hdr;
    if (paylen) memcpy(out+5, pay, paylen);
    return 5 + paylen;
}

/* 3-byte Annex-B start-code variant: 00 00 01 <hdr> <payload...> into out. */
static size_t mk_nal3(uint8_t *out, uint8_t hdr, const uint8_t *pay, size_t paylen) {
    out[0]=0; out[1]=0; out[2]=1; out[3]=hdr;
    if (paylen) memcpy(out+4, pay, paylen);
    return 4 + paylen;
}

static void test_init_and_empty_flush(void) {
    printf("test_au_asm: init + empty flush is a no-op\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL);
    cap_reset();
    au_asm_flush(&a);           /* nothing buffered */
    assert(g_emit_n == 0);
    assert(a.aus_emitted == 0);
    printf("  ok\n");
}

/* NAL header nibbles: nal_ref_idc<<5 | type. VCL types: 1 (non-IDR P), 5 (IDR).
 * A slice's first RBSP byte with bit7 set => first_mb_in_slice==0 (AU start). */
#define H_P_SLICE  0x41   /* ref_idc=2, type=1  (P slice)  */
#define H_IDR      0x65   /* ref_idc=3, type=5  (IDR slice)*/
#define H_SPS      0x67   /* type=7 non-VCL */
#define H_PPS      0x68   /* type=8 non-VCL */
#define FMB0 0x80         /* first_mb_in_slice==0 marker (bit7 of RBSP byte 0) */
#define FMBx 0x10         /* first_mb_in_slice!=0 (some later slice)          */

static void feed_nal(AuAsm *a, uint8_t hdr, uint8_t rbsp0) {
    uint8_t nal[16]; uint8_t pay[1] = { rbsp0 };
    size_t n = mk_nal(nal, hdr, pay, 1);
    au_asm_feed(a, nal, n);
}

static void test_two_p_frames_split_on_au_start(void) {
    printf("test_au_asm: two single-slice P frames split into two AUs\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    feed_nal(&a, H_P_SLICE, FMB0);   /* frame N slice-0 (au start) */
    assert(g_emit_n == 0);           /* nothing emitted yet        */
    feed_nal(&a, H_P_SLICE, FMB0);   /* frame N+1 slice-0 -> closes N */
    assert(g_emit_n == 1);
    au_asm_flush(&a);                /* wire idle -> closes N+1     */
    assert(g_emit_n == 2);
    /* each emitted AU is exactly one 6-byte NAL (00 00 00 01 hdr rbsp0) */
    assert(g_emit[0].len == 6 && g_emit[0].buf[4] == H_P_SLICE);
    assert(g_emit[1].len == 6);
    printf("  ok\n");
}

static void test_multislice_frame_stays_one_au(void) {
    printf("test_au_asm: multi-slice frame is a single AU\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    feed_nal(&a, H_P_SLICE, FMB0);   /* slice 0 (au start) */
    feed_nal(&a, H_P_SLICE, FMBx);   /* slice 1 (not au start) */
    feed_nal(&a, H_P_SLICE, FMBx);   /* slice 2 */
    assert(g_emit_n == 0);
    feed_nal(&a, H_P_SLICE, FMB0);   /* next frame -> closes the 3-slice AU */
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 18);     /* 3 * 6 bytes */
    printf("  ok\n");
}

static void feed_raw(AuAsm *a, uint8_t hdr) {   /* non-VCL, no rbsp needed */
    uint8_t nal[8]; size_t n = mk_nal(nal, hdr, NULL, 0);
    au_asm_feed(a, nal, n);
}

static void test_sps_pps_group_with_next_idr(void) {
    printf("test_au_asm: SPS+PPS precede and group with the IDR AU\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    feed_nal(&a, H_P_SLICE, FMB0);   /* frame N */
    feed_raw(&a, H_SPS);             /* params arrive after N's slice ... */
    feed_raw(&a, H_PPS);
    assert(g_emit_n == 0);           /* SPS/PPS alone do NOT close N here  */
    /* On a real wire the idle gap closes N BEFORE the params; emulate: */
    /* (here params landed pre-gap, so they ride with N -- still correct) */
    feed_nal(&a, H_IDR, FMB0);       /* IDR au-start closes N (which now
                                        includes SPS/PPS at its tail)      */
    assert(g_emit_n == 1);
    au_asm_flush(&a);                /* close the IDR AU */
    assert(g_emit_n == 2);
    assert(g_emit[1].buf[4] == H_IDR);
    printf("  ok\n");
}

static void test_flush_only_emits_with_vcl(void) {
    printf("test_au_asm: flush of a params-only buffer keeps it (no VCL)\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    feed_raw(&a, H_SPS);
    feed_raw(&a, H_PPS);
    au_asm_flush(&a);                /* no VCL yet -> nothing emitted */
    assert(g_emit_n == 0);
    feed_nal(&a, H_IDR, FMB0);       /* IDR joins the retained SPS/PPS */
    au_asm_flush(&a);
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 5 + 5 + 6); /* SPS(5)+PPS(5)+IDR(6) */
    printf("  ok\n");
}

static void test_partial_nal_passthrough(void) {
    printf("test_au_asm: a truncated slice NAL is buffered verbatim\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    /* a partial slice: start code + hdr + 1 rbsp byte (lan_reasm prefix) */
    feed_nal(&a, H_P_SLICE, FMB0);
    feed_nal(&a, H_P_SLICE, FMB0);   /* next frame closes the partial one */
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 6);      /* verbatim, no repair */
    printf("  ok\n");
}

static void test_overflow_drops_au(void) {
    printf("test_au_asm: an AU exceeding AU_ASM_CAP is dropped and counted\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    static uint8_t big[AU_ASM_CAP];
    big[0]=0; big[1]=0; big[2]=0; big[3]=1; big[4]=H_P_SLICE; big[5]=FMB0;
    au_asm_feed(&a, big, AU_ASM_CAP); /* au-start slice fills the buffer: len==cap */
    big[5]=FMBx;                      /* continuation slice of the SAME AU (no reset) */
    au_asm_feed(&a, big, AU_ASM_CAP); /* cap + cap > cap -> whole AU dropped */
    assert(a.overflow_drops >= 1);
    printf("  ok\n");
}

static void test_overflow_latch_drops_rest_of_au(void) {
    printf("test_au_asm: overflow latch drops the rest of the AU (no headless emit)\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    /* An au-start slice larger than the cap overflows on its very first feed. */
    static uint8_t big[AU_ASM_CAP + 16];
    big[0]=0; big[1]=0; big[2]=0; big[3]=1; big[4]=H_P_SLICE; big[5]=FMB0;
    au_asm_feed(&a, big, sizeof big);   /* overflow -> drop + latch dropping */
    assert(a.overflow_drops == 1);
    /* Orphaned continuation slices of the dropped AU must be swallowed, not
     * buffered into a headless AU. */
    feed_nal(&a, H_P_SLICE, FMBx);      /* continuation slice (not au-start) */
    feed_nal(&a, H_P_SLICE, FMBx);      /* continuation slice */
    assert(g_emit_n == 0);              /* nothing headless emitted */
    /* The next au-start clears the latch and begins a clean new AU. */
    feed_nal(&a, H_P_SLICE, FMB0);      /* fresh AU start */
    assert(g_emit_n == 0);              /* still buffering the clean AU */
    au_asm_flush(&a);                   /* wire idle -> emit the clean AU */
    assert(g_emit_n == 1);              /* ONLY the clean new AU */
    assert(g_emit[0].len == 6);         /* one 6-byte au-start NAL, nothing prepended */
    assert(a.overflow_drops == 1);      /* the dropped AU counted exactly once */
    printf("  ok\n");
}

static void test_three_byte_start_code_equivalent(void) {
    printf("test_au_asm: a 3-byte start code is classified/assembled like the 4-byte form\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    /* frame N via 4-byte start code */
    feed_nal(&a, H_P_SLICE, FMB0);
    assert(g_emit_n == 0);
    /* frame N+1 via 3-byte start code: still an au-start VCL -> closes N */
    uint8_t nal3[16]; uint8_t pay[1] = { FMB0 };
    size_t n3 = mk_nal3(nal3, H_P_SLICE, pay, 1);   /* 00 00 01 hdr rbsp0 -> 5 bytes */
    au_asm_feed(&a, nal3, n3);
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 6);          /* the 4-byte NAL, verbatim */
    au_asm_flush(&a);                    /* close the 3-byte-framed AU */
    assert(g_emit_n == 2);
    assert(g_emit[1].len == 5);          /* the 3-byte-framed NAL, verbatim */
    assert(g_emit[1].buf[3] == H_P_SLICE);
    printf("  ok\n");
}

static void test_start_code_only_nal_no_split(void) {
    printf("test_au_asm: a start-code-only NAL (no header) buffers verbatim, no split\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    feed_nal(&a, H_P_SLICE, FMB0);       /* au-start slice -> has_vcl */
    uint8_t sc_only[4] = { 0, 0, 0, 1 }; /* start code, no header byte */
    au_asm_feed(&a, sc_only, sizeof sc_only);  /* not VCL, not au-start -> no split */
    assert(g_emit_n == 0);               /* did NOT trigger a premature split */
    feed_nal(&a, H_P_SLICE, FMB0);       /* next real au-start closes the AU */
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 6 + 4);      /* slice(6) + start-code-only(4), verbatim */
    printf("  ok\n");
}

static void test_no_start_code_nal_opaque_no_split(void) {
    printf("test_au_asm: a NAL with no start code is opaque, buffered verbatim, no split\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    feed_nal(&a, H_P_SLICE, FMB0);       /* au-start slice -> has_vcl */
    uint8_t raw[3] = { 0xAA, 0xBB, 0xCC };  /* no Annex-B start code */
    au_asm_feed(&a, raw, sizeof raw);    /* hdr_off==0 -> non-VCL/opaque, no split */
    assert(g_emit_n == 0);
    feed_nal(&a, H_P_SLICE, FMB0);       /* next au-start closes the AU */
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 6 + 3);      /* slice(6) + opaque(3), verbatim */
    printf("  ok\n");
}

static void test_flush_partial_frame_drops_headless_continuation(void) {
    printf("test_au_asm: a partial-flush tail is dropped, not emitted headless\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    feed_nal(&a, H_P_SLICE, FMB0);   /* frame N slice0 (au-start) */
    feed_nal(&a, H_P_SLICE, FMBx);   /* slice1 */
    feed_nal(&a, H_P_SLICE, FMBx);   /* slice2 */
    au_asm_flush(&a);                /* wire idle -> emit PARTIAL [s0,s1,s2] */
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 18);
    /* the frame's remaining slices arrive late: VCL, non-au-start -> orphaned */
    feed_nal(&a, H_P_SLICE, FMBx);   /* slice3 */
    feed_nal(&a, H_P_SLICE, FMBx);   /* slice4 */
    assert(g_emit_n == 1);           /* the headless tail is NOT emitted */
    /* frame N+1: SPS/PPS (must survive the drop) then its au-start slice */
    feed_raw(&a, H_SPS);
    feed_raw(&a, H_PPS);
    feed_nal(&a, H_P_SLICE, FMB0);   /* au-start */
    au_asm_flush(&a);
    assert(g_emit_n == 2);               /* exactly 2 AUs total */
    assert(g_emit[1].len == 5 + 5 + 6);  /* SPS(5)+PPS(5)+slice0(6) */
    assert(g_emit[1].buf[4] == H_SPS);   /* SPS present at the head of AU 2 */
    printf("  ok\n");
}

static void test_flush_then_sps_pps_not_dropped(void) {
    printf("test_au_asm: SPS/PPS right after a flush are preserved (not dropped)\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL); cap_reset();
    feed_nal(&a, H_P_SLICE, FMB0);   /* frame N slice0 (au-start) */
    au_asm_flush(&a);                /* emit [s0] */
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 6);
    feed_raw(&a, H_SPS);             /* non-VCL: NOT an orphaned tail */
    feed_raw(&a, H_PPS);
    feed_nal(&a, H_IDR, FMB0);       /* au-start IDR */
    au_asm_flush(&a);
    assert(g_emit_n == 2);
    assert(g_emit[1].len == 5 + 5 + 6);  /* SPS+PPS+IDR all present */
    assert(g_emit[1].buf[4] == H_SPS);
    printf("  ok\n");
}

/* --- Trailer NAL (type 12 filler) terminates the AU ----------------------
 * The TX appends a 6-byte filler NAL after each frame's last slice (IMP-1).
 * It is the frame's end-marker: emit the AU the moment it arrives, instead
 * of holding the frame ~33 ms for the NEXT frame's au-start. This also
 * empties the buffer before any idle-flush can fire mid-frame (the
 * bottom-half-corruption RCA, 2026-07-05: a mid-frame flush presented the
 * top slices and discarded the bottom ones). */
#define H_FILLER 0x0C   /* ref_idc=0, type=12 (filler data) */

static void test_trailer_terminates_au(void) {
    printf("test_au_asm: trailer filler NAL emits the AU immediately\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL);
    cap_reset();
    feed_nal(&a, H_P_SLICE, FMB0);   /* slice 0    */
    feed_nal(&a, H_P_SLICE, FMBx);   /* tail slice */
    assert(g_emit_n == 0);           /* no next-AU evidence yet */
    feed_nal(&a, H_FILLER, 0x80);    /* frame trailer -> AU complete NOW */
    assert(g_emit_n == 1);
    assert(g_emit[0].len == 3 * 6);  /* both slices + trailer, all 6 B each */
    assert(a.len == 0);              /* buffer empty after emit */
    /* Next frame flows normally: its own trailer emits it, no double-emit. */
    feed_nal(&a, H_P_SLICE, FMB0);
    feed_nal(&a, H_FILLER, 0x80);
    assert(g_emit_n == 2);
    assert(g_emit[1].len == 2 * 6);
    printf("  ok\n");
}

static void test_trailer_without_vcl_keeps_params(void) {
    printf("test_au_asm: trailer with no VCL buffered does not emit\n");
    AuAsm a; au_asm_init(&a, cap_emit, NULL);
    cap_reset();
    feed_nal(&a, H_SPS, 0x00);       /* params-only buffer */
    feed_nal(&a, H_FILLER, 0x80);    /* stray trailer: nothing to terminate */
    assert(g_emit_n == 0);
    feed_nal(&a, H_IDR, FMB0);       /* the IDR the params belong to */
    feed_nal(&a, H_FILLER, 0x80);    /* its trailer emits SPS+stray+IDR+trailer */
    assert(g_emit_n == 1);
    assert(g_emit[0].buf[4] == H_SPS);   /* params retained at the front */
    printf("  ok\n");
}

int main(void) {
    test_init_and_empty_flush();
    test_trailer_terminates_au();
    test_trailer_without_vcl_keeps_params();
    test_two_p_frames_split_on_au_start();
    test_multislice_frame_stays_one_au();
    test_sps_pps_group_with_next_idr();
    test_flush_only_emits_with_vcl();
    test_partial_nal_passthrough();
    test_overflow_drops_au();
    test_overflow_latch_drops_rest_of_au();
    test_three_byte_start_code_equivalent();
    test_start_code_only_nal_no_split();
    test_no_start_code_nal_opaque_no_split();
    test_flush_partial_frame_drops_headless_continuation();
    test_flush_then_sps_pps_not_dropped();
    printf("all au_asm tests passed\n");
    return 0;
}
