#ifndef LR2021_RX_WALK_H
#define LR2021_RX_WALK_H

/* RX multi-packet stream walker (coalesced-RX_DONE handling).
 *
 * RX_DONE is a single coalescing IRQ status bit, and GetRxPktLength returns
 * the payload length of the LAST received packet only (DS 7.3.10). In
 * RX-continuous mode all payloads are appended to the shared 256 B FIFO
 * (DS 5.3.1), so when a second packet finishes while the host is still
 * servicing the previous RX_DONE, the FIFO holds MULTIPLE packets and pkt_len
 * no longer describes the packet at the FIFO read pointer. The old drain path
 * trusted pkt_len unconditionally -- HW-measured (COALESCE_PROBE, 2026-07-05):
 * the head packet got misframed at 0.34/s in plain production traffic and at
 * 1.19/s with the ~5 B frame trailer on air, ~1:1 with damaged AUs.
 *
 * The walker frames each packet by its OWN in-band header instead:
 *   - video/FEC packets use the FIXED protocol format flags(1)+LE16 len(2)
 *     (self-framing; carried in-band even though the chip runs DYNAMIC FLRC);
 *   - the chip pkt_len is still used for the classic exact-match / stray-byte
 *     disambiguation (identical to the old lr2021_rx_frame_offset v0/v1 test);
 *   - DYN records (OSD grid; no in-band length) and unrecognizable bytes fall
 *     back to the legacy pkt_len path, preserving old behavior exactly.
 *
 * Completeness gating lives in the CALLER: packets are serial on air, so the
 * head packet has fully arrived iff acc + GetRxFifoLevel() >= its extent; a
 * NEED whose byte count exceeds the FIFO level means the head is the packet
 * currently in flight -- keep the accumulator and resume on its own RX_DONE.
 *
 * Pure (no kernel primitives): shared verbatim by the driver and the host
 * unit test tests/driver/test_rx_walk.c, like lr2021_frame_sync.h. */

#include <linux/types.h>
#include "lr2021_frame_sync.h"

/* Mirror of PROTOCOL_MAX_PAYLOAD (common/protocol.h): FIXED header is
 * flags(1)+len(2) = 3, on-air max 511 -> payload <= 508. The driver cannot
 * include userspace headers; tests/driver/test_rx_walk.c pins this mirror. */
#define LR2021_WALK_MAX_PAYLOAD 508u

/* Minimum credible in-band payload length. The smallest real packet on this
 * link is the 5-byte frame trailer (payload 2). LE16 values 0..1 are treated
 * as noise -> LEGACY, not a walk. */
#define LR2021_WALK_MIN_PAYLOAD 2u

enum lr2021_rx_walk_act {
    LR2021_WALK_EMIT   = 0,  /* emit *n_out bytes at buf[*off_out]           */
    LR2021_WALK_NEED   = 1,  /* need *n_out more bytes appended (level-gate!) */
    LR2021_WALK_LEGACY = 2,  /* not in-band framed: legacy pkt_len semantics  */
};

/* True if buf[o..o+2] parses as a credible in-band FIXED header: TYPE/reserved
 * bit clear and LE16 payload length within protocol bounds. */
static inline int lr2021_walk_hdr_ok(const __u8 *buf, int o, __u16 *len_out) {
    __u16 pay = (__u16)buf[o + 1] | ((__u16)buf[o + 2] << 8);
    if ((buf[o] & LR2021_FLAGS_RESERVED_MASK) != 0u) return 0;
    if (pay < LR2021_WALK_MIN_PAYLOAD || pay > LR2021_WALK_MAX_PAYLOAD) return 0;
    if (len_out) *len_out = (__u16)(pay + 3u);
    return 1;
}

/* Decide the next action on the accumulated RX stream.
 *
 *   buf[0..acc)  accumulated stream bytes (head of the FIFO byte stream)
 *   pkt_len      chip-reported length of the LAST completed packet (2..510,
 *                validated by the caller)
 *
 * EMIT:   the head packet is fully accumulated; push buf[*off_out ..
 *         *off_out + *n_out) and consume *off_out + *n_out bytes.
 * NEED:   *n_out more bytes are required to frame/complete the head packet.
 *         The caller must gate on the FIFO level: level < n means the head is
 *         still in flight -> keep the accumulator, resume on the next IRQ.
 * LEGACY: the head is not in-band framed (DYN/OSD record, linktest format, or
 *         garbage). Caller falls back to the old pkt_len-based single-packet
 *         path (frame-offset heuristic + stray tail) and ends the walk. */
static inline int lr2021_rx_walk_step(const __u8 *buf, __u16 acc, __u16 pkt_len,
                                      int *off_out, __u16 *n_out) {
    __u16 want, len0, len1;
    int have4 = (acc >= 4);

    /* Need 4 bytes to evaluate both candidate offsets (v1 reads buf[3]). */
    if (!have4) { *n_out = (__u16)(4 - acc); return LR2021_WALK_NEED; }

    /* 1. Exact-match against the chip length, offsets 0 and 1 -- the same
     *    16-bit test the old lr2021_rx_frame_offset used, still the strongest
     *    signal when the head IS the pkt_len packet (single packet, or the
     *    final packet of a coalesced batch). */
    want = (__u16)(pkt_len - 3);
    if (pkt_len >= 4) {
        int v0 = ((__u16)(buf[1] | ((__u16)buf[2] << 8)) == want);
        int v1 = ((__u16)(buf[2] | ((__u16)buf[3] << 8)) == want);
        if (v0 && !v1) {
            if (acc >= pkt_len) { *off_out = 0; *n_out = pkt_len; return LR2021_WALK_EMIT; }
            *n_out = (__u16)(pkt_len - acc); return LR2021_WALK_NEED;
        }
        if (v1 && !v0) {
            /* stray byte prepended: packet occupies [1 .. 1+pkt_len) */
            if (acc >= (__u16)(pkt_len + 1)) { *off_out = 1; *n_out = pkt_len; return LR2021_WALK_EMIT; }
            *n_out = (__u16)((__u16)(pkt_len + 1) - acc); return LR2021_WALK_NEED;
        }
        if (v0 && v1) return LR2021_WALK_LEGACY;   /* ambiguous -- old heuristic */
    }

    /* 2. DYN pivid record (OSD grid / retired telemetry): recognizable by
     *    signature but carries NO in-band length -> only pkt_len can frame it.
     *    Hand back to the legacy path (correct whenever it is the pkt_len
     *    packet, i.e. the common non-coalesced case). */
    if (lr2021_dyn_frame_offset(buf, acc, (void *)0))
        return LR2021_WALK_LEGACY;

    /* 3. In-band framing: the head is an earlier packet of a coalesced batch
     *    (pkt_len belongs to a LATER packet). Frame it by its own LE16. */
    if (lr2021_walk_hdr_ok(buf, 0, &len0)) {
        if (acc >= len0) { *off_out = 0; *n_out = len0; return LR2021_WALK_EMIT; }
        *n_out = (__u16)(len0 - acc); return LR2021_WALK_NEED;
    }
    /* stray variant: buf[0] unparseable but buf[1..] frames cleanly */
    if (lr2021_walk_hdr_ok(buf, 1, &len1)) {
        if (acc >= (__u16)(len1 + 1)) { *off_out = 1; *n_out = len1; return LR2021_WALK_EMIT; }
        *n_out = (__u16)((__u16)(len1 + 1) - acc); return LR2021_WALK_NEED;
    }

    return LR2021_WALK_LEGACY;
}

#endif /* LR2021_RX_WALK_H */
