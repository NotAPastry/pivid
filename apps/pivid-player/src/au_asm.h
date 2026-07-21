/* apps/pivid-player/src/au_asm.h -- pure Annex-B access-unit assembler.
 *
 * Fed framed NALs (each begins with a 3- or 4-byte Annex-B start code) in
 * stream order, exactly as lan_reasm emits them. Emits one complete AU
 * (contiguous Annex-B, start codes preserved) via a callback when the next AU
 * begins (a VCL NAL with first_mb_in_slice==0 arriving while the buffer already
 * holds a VCL NAL) or when au_asm_flush() is called on a wire-idle gap.
 *
 * Pure state machine: no sockets, no libav. Mirrors lan_reasm's testability. */
#ifndef PLAYER_AU_ASM_H
#define PLAYER_AU_ASM_H

#include <stddef.h>
#include <stdint.h>

/* One AU ceiling. A 480p IDR is well under this; the overflow branch is a
 * defensive drop-and-count, not a normal path. */
#define AU_ASM_CAP (256u * 1024u)

/* emit: a complete assembled AU. Valid only for the call; copy to retain. */
typedef void (*AuAsm_EmitCb)(const uint8_t *au, size_t len, void *user);

typedef struct {
    AuAsm_EmitCb emit;
    void        *user;
    uint8_t      buf[AU_ASM_CAP];
    size_t       len;            /* bytes currently buffered                  */
    int          has_vcl;        /* buffer holds >=1 VCL NAL                   */
    int          dropping;       /* over-cap AU: swallow its tail until next   */
                                 /* au-start, so no headless AU is emitted     */
    int          post_flush;     /* last op was a VCL-emitting flush; a VCL    */
                                 /* non-au-start next is a partial-frame tail  */
    /* counters */
    uint64_t     nals_in;
    uint64_t     aus_emitted;
    uint64_t     overflow_drops; /* NAL didn't fit AU_ASM_CAP -> AU dropped    */
} AuAsm;

void au_asm_init(AuAsm *a, AuAsm_EmitCb emit, void *user);

/* Feed one framed NAL (start-code prefixed). */
void au_asm_feed(AuAsm *a, const uint8_t *nal, size_t len);

/* Emit the buffered AU now if it holds a VCL NAL (wire-idle gap / EOF). A
 * buffer with only non-VCL NALs (SPS/PPS/SEI) is kept for the next AU. */
void au_asm_flush(AuAsm *a);

#endif
