#ifndef LR2021_FRAME_SYNC_H
#define LR2021_FRAME_SYNC_H

/* RX SPI-frame self-sync.
 *
 * The LR2021 direct-read FIFO frame (datasheet §5.4.1.3 / fig 5-4) returns
 * Stat(15:0) followed by the FIFO bytes in a single SPI frame. At 16 MHz the
 * chip occasionally has not fetched Data0 by the third clocked byte and emits
 * one stale "stray" byte before the real payload. The kernel reads exactly
 * pkt_len body bytes, runs these helpers to decide (per packet) whether the
 * real protocol frame starts at offset 0 (no stray) or 1 (stray prepended),
 * and issues a single follow-up FIFO read for the tail byte ONLY when a stray
 * is detected -- so it never over-reads into the next packet (that +1
 * unconditional over-read was the bug2 theft, HW-measured ~3.7%).
 *
 * The decision is per-PACKET, not locked once per module load: the stray count
 * varies packet-to-packet, so a single locked offset misframes the minority
 * that differ (that was project_rx_misframe_fec_fabrication). The post-opcode
 * delay added to lr2021_spi_fifo_read removes the stray at the source; this
 * self-sync is the safety net for any residual.
 *
 * Pure (no kernel primitives), so it is shared verbatim by the driver and the
 * host unit test (tests/test_frame_sync.c), exactly like lr2021_ring.h. */

/* <linux/types.h> provides __u8 both in-kernel and for the host unit test
 * (built on Linux), so no separate userspace include path is needed. */
#include <linux/types.h>

/* Header-length sanity ceiling for the self-sync heuristic. DELIBERATELY a
 * single-byte test (see lr2021_frame_offset_valid): the byte at p[off+1] must be
 * <= 253. This is NOT PROTOCOL_MAX_PAYLOAD (508) -- it is intentionally a 1-byte
 * discriminator that works for BOTH header formats riding this driver:
 *   - pivid-linktest uses a literal 1-byte length (lt_packet.c), and the byte at
 *     p[off+2] is its pattern id, NOT a length high byte;
 *   - common/protocol.c uses an LE16 length that never exceeds 508, so its LOW
 *     byte (<= 253 for any real-but-not-509 packet) is all the stray-byte test
 *     needs.
 * Do NOT "raise" this to 508 / read the LE16 high byte: p[off+2] is the next
 * header field, so doing so misframes nearly every packet. MEASURED 2026-06-05:
 * that change dropped linktest header_ok from 6979 to 328 over an identical
 * sweep. The post-opcode delay in lr2021_spi_fifo_read provides the real
 * determinism; this check is only a fallback. */
#define LR2021_PROTO_MAX_PAYLOAD 253

/* Reserved flag bits that must be 0 in a valid protocol header.
 * Flag map (common/protocol.h): START=1<<7 END=1<<6 SEQ=bits5..2 FEC=1<<1
 * reserved=1<<0. ONLY bit 0 is reserved -- bit 1 is PROTO_FLAG_FEC and is set
 * on FEC packets, so the self-sync invariant must not require it to be 0.
 * (The original driver used 0x03 and would misframe FEC packets once enabled.)
 *
 * NOTE: this invariant weakens as flag bits are allocated (bit 0 is earmarked
 * for the planned MSP/OSD TYPE bit). The real determinism comes from the
 * post-opcode delay in lr2021_spi_fifo_read; this check is a fallback. */
#define LR2021_FLAGS_RESERVED_MASK 0x01u

/* Running per-direction tally used to learn the dominant offset, so an
 * ambiguous packet (both/neither offset validates) follows the established
 * framing instead of guessing. Zero-initialised == no votes, sticky 0. */
struct lr2021_frame_sync {
    unsigned int off0_votes;
    unsigned int off1_votes;
};

/* True if the bytes at p[off..off+1] look like a valid header start: flags has
 * no reserved bit set and the single length byte is <= LR2021_PROTO_MAX_PAYLOAD.
 * Single-byte BY DESIGN -- see the LR2021_PROTO_MAX_PAYLOAD note for why reading
 * p[off+2] as a length high byte is wrong here. `p` must have at least off+2
 * readable bytes. */
static inline int lr2021_frame_offset_valid(const __u8 *p, int off) {
    return ((p[off] & LR2021_FLAGS_RESERVED_MASK) == 0u)
        && (p[off + 1] <= LR2021_PROTO_MAX_PAYLOAD);
}

/* DYNAMIC-mode pivid record signature, for frame-offset disambiguation.
 *
 * A self-contained DYNAMIC record (no LE16 length) carries flags = START|END|
 * TYPE = 0xC1 at the true offset, then payload[0] = a small protocol version.
 * Two such records ride this link:
 *   - OSD-grid keyframe FRAGMENT (apps/pivid-tx msp_poll.c + common/osd_grid.c):
 *     payload[0] = OSD_GRID_VERSION = 1.  VARIABLE length (one fragment is
 *     ~9..265 B), so this disambiguation MUST NOT gate on a fixed pkt_len.
 *   - the now-retired value telemetry record: length 36, payload[0] = 2.
 * Constants are mirrored here because the driver can't include the userspace
 * common/{osd_grid,protocol}.h; tests/test_frame_sync.c carries _Static_asserts
 * against the real values so this mirror can't silently drift again. */
#define LR2021_TELEM_PKT_LEN     36u
#define LR2021_TELEM_FLAGS       0xC1u   /* START|END|TYPE, FEC=0, SEQ=0 */
#define LR2021_TELEM_VERSION     0x02u
#define LR2021_OSD_GRID_VERSION  0x01u

/* Disambiguate the frame offset for a DYNAMIC pivid record by its flags+version
 * signature. A DYNAMIC record has no LE16 length, so the video pkt_len check
 * can't frame it, and the single-byte heuristic (lr2021_frame_offset_valid)
 * REJECTS it at the true offset -- the 0xC1 flags byte's TYPE bit (bit 0) trips
 * the reserved-bit-0 test, leaving the record permanently FC_AMBIG and stuck on
 * the video-learned sticky offset (HW-measured: 100% telemetry misframe
 * 2026-06-19; 100% OSD-grid misframe / osd_frags_rx=0 2026-06-20 before this
 * length-agnostic generalization). Keyed on the SIGNATURE not pkt_len because
 * OSD-grid fragments are variable length. Returns 1 and writes 0 or 1 to
 * *off_out when exactly one candidate offset carries the signature; returns 0
 * otherwise (no match -> caller falls back to the heuristic). The 0xC1 flags
 * byte can sit at only one position, so a both-offsets match is impossible.
 * Video (FIXED mode) is framed by the LE16 check before reaching here and its
 * flags byte has bit0=0 (never 0xC1), so this can't poach a clean video packet.
 * `p` must have at least 3 readable bytes. The caller reads pkt_len body bytes
 * into a >=512 B buffer, so for any real record (pkt_len>=3) p[0..2] are valid;
 * only the degenerate pkt_len==2 noise packet leaves p[2] as stale buffer
 * content (in-bounds, never OOB), and such a packet fails protocol parse and is
 * dropped regardless of the decision here. */
static inline int lr2021_dyn_frame_offset(const __u8 *p, __u16 pkt_len,
                                          int *off_out) {
    int v0, v1;
    (void)pkt_len;   /* variable-length: disambiguate by signature, not length */
    v0 = (p[0] == LR2021_TELEM_FLAGS &&
          (p[1] == LR2021_OSD_GRID_VERSION || p[1] == LR2021_TELEM_VERSION));
    v1 = (p[1] == LR2021_TELEM_FLAGS &&
          (p[2] == LR2021_OSD_GRID_VERSION || p[2] == LR2021_TELEM_VERSION));
    if (v0 && !v1) { if (off_out) *off_out = 0; return 1; }
    if (v1 && !v0) { if (off_out) *off_out = 1; return 1; }
    return 0;
}

/* Classification of a per-packet framing decision, for diagnostics. The
 * kernel counts these (via debugfs) to measure the stray rate directly at its
 * source -- an RF-independent probe, since CRC-failed packets never reach the
 * frame-offset decision (see lr2021_drain_rx_packet). Purely observational:
 * it does NOT change which offset is chosen. */
enum lr2021_frame_class {
    LR2021_FC_OFF0  = 0,  /* unambiguous: only offset 0 valid (no stray)        */
    LR2021_FC_OFF1  = 1,  /* unambiguous: only offset 1 valid (stray detected)  */
    LR2021_FC_AMBIG = 2,  /* both or neither valid -> followed learned dominant.
                           * A real stray hiding here is the residual-misframe
                           * risk: ~50% of strays whose stray byte has bit0=0. */
};

/* Decide the frame offset (0 or 1) for one packet, update the tally, AND
 * report which category the decision fell into via *class_out.
 *
 *   - exactly one offset valid  -> use it, count a vote, FC_OFF0/FC_OFF1
 *   - both or neither valid      -> ambiguous; follow the learned dominant
 *                                   offset (no vote cast), FC_AMBIG
 *
 * `p` must have at least 3 readable bytes. The kernel reads pkt_len body bytes
 * into a >=512 B buffer, so p[0..2] are valid for any real record (pkt_len>=3);
 * a degenerate pkt_len==2 packet leaves p[2] stale-but-in-bounds and is dropped
 * downstream. Returns 0 or 1; writes the classification to *class_out (may be
 * NULL if the caller doesn't need it). */
static inline int lr2021_frame_sync_offset_classified(struct lr2021_frame_sync *s,
                                                       const __u8 *p,
                                                       enum lr2021_frame_class *class_out) {
    int v0 = lr2021_frame_offset_valid(p, 0);
    int v1 = lr2021_frame_offset_valid(p, 1);

    if (v0 && !v1) { s->off0_votes++; if (class_out) *class_out = LR2021_FC_OFF0; return 0; }
    if (v1 && !v0) { s->off1_votes++; if (class_out) *class_out = LR2021_FC_OFF1; return 1; }

    /* Ambiguous: follow the dominant offset (default 0 when tied/unseen). */
    if (class_out) *class_out = LR2021_FC_AMBIG;
    return (s->off1_votes > s->off0_votes) ? 1 : 0;
}

/* Thin wrapper for callers/tests that only need the offset. */
static inline int lr2021_frame_sync_offset(struct lr2021_frame_sync *s,
                                           const __u8 *p) {
    return lr2021_frame_sync_offset_classified(s, p, (void *)0);
}

/* Convenience for the diagnostic test/kernel: returns the class, writes the
 * offset to *offset_out. */
static inline enum lr2021_frame_class
lr2021_frame_sync_classify(struct lr2021_frame_sync *s, const __u8 *p,
                           int *offset_out) {
    enum lr2021_frame_class c = LR2021_FC_AMBIG;
    int off = lr2021_frame_sync_offset_classified(s, p, &c);
    if (offset_out) *offset_out = off;
    return c;
}

#endif /* LR2021_FRAME_SYNC_H */
