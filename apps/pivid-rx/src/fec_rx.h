#ifndef RX_FEC_RX_H
#define RX_FEC_RX_H

#include <stdint.h>
#include <stddef.h>
#include "fec.h"

/* Emit one recovered inner record ([flags][len][payload]) downstream to reasm. */
typedef void (*FecRx_Emit)(const uint8_t *record, size_t len, void *user);

#define FEC_RX_TIMEOUT_NS 500000000ull   /* match REASM_TIMEOUT_NS */

/* Concurrently-open deinterleave slots. Production keeps 4; the offline
 * block-interleaver study build overrides this (-DFEC_RX_SLOTS=32) so it can hold
 * D concurrently-open blocks (>= max swept depth + margin). slot_for +
 * drain_ordered are count-agnostic (they iterate FEC_RX_SLOTS), so a larger count
 * is pure capacity -- no logic change. */
#ifndef FEC_RX_SLOTS
#define FEC_RX_SLOTS 4        /* production unchanged */
#endif

typedef struct {
    int      used;
    int      closed;               /* fully received/decoded, awaiting ordered flush */
    uint8_t  block_id;
    int      k, n, have;
    uint8_t  present[FEC_K_MAX + FEC_M_MAX];
    /* Full FEC_K_MAX+FEC_M_MAX capacity per slot is INTENTIONAL: it decouples the
     * RX buffer from the TX k_bound knob (RX must accept any legal k/n it sees).
     * ~58 KB/slot * FEC_RX_SLOTS == ~235 KB total, allocated static (BSS) in the
     * RX main loop -- do NOT shrink this to chase memory. */
    uint8_t  sym[FEC_K_MAX + FEC_M_MAX][FEC_SYMBOL_LEN];
    uint64_t last_progress_ns;
#ifdef LATPROBE
    uint64_t first_sym_ns;   /* S5: arrival of this block's first symbol */
#endif
} FecRxSlot;

typedef struct {
    FecRxSlot slot[FEC_RX_SLOTS];
    /* Set in slot_emit on every flush; consumed by the ordered-flush / interleave
     * logic once interleave lands. Not dead -- the serial id of the most
     * recently drained block lets the deinterleaver place an incoming column. */
    int       have_last_emit;      /* last_emit_bid is valid */
    uint8_t   last_emit_bid;       /* serial id of the last block flushed to reasm */

    FecRx_Emit emit;
    void      *user;

    uint64_t fec_blocks;
    uint64_t fec_recovered;        /* blocks that needed + achieved RS recovery */
    uint64_t fec_unrecoverable;    /* blocks with < k survivors */
    /* DIAGNOSTIC (fec_unrec root-cause split): partition every unrecoverable
     * block by whether it had enough symbols to even attempt reconstruction.
     *   unrec_short   = have < k  -> bursty ERASURE loss (loss > parity m); the
     *                   lever is m_ref / interleave, not CRC width.
     *   unrec_present = have >= k -> k symbols WERE present yet RS decode failed
     *                   -> a "present" symbol is corrupt (CRC-16 escape) -> IMP-5
     *                   (widen the FLRC CRC). Under pure erasure loss this is 0.
     *   unrec_deficit = running sum of (k - have) over short blocks -> mean
     *                   symbols-short tells apart "just over the edge" vs "wiped". */
    uint64_t unrec_short;
    uint64_t unrec_present;
    uint64_t unrec_deficit;

    /* DIAGNOSTIC: misframe detector. TX emits block_ids monotonically (+1 per
     * block); a new block whose id is NOT a small forward step of the previous
     * one is a fabricated block from a corrupted (byte-shifted) FEC sub-header.
     * Under clean operation bid_noninc stays ~0; it explodes if the kernel SPI
     * read is misframing. */
    uint8_t  last_open_bid;
    int      last_open_bid_valid;
    uint64_t bid_noninc;

    /* Pre-demux drop counters: a malformed (short) record, a pair-header that
     * fails the sanity gate, or a record whose parsed payload length does not
     * EQUAL count full symbols (strict ==; OOB + misframe guard). A TX/RX mode
     * mismatch is not bit-cross-checked -- it surfaces here as mass length drops. */
    uint32_t drops_malformed, drops_sanity, drops_len;
    uint32_t drops_inner_len;   /* recovered/forged symbol with inner len > 249: reject */
    uint32_t drops_parse;   /* outer-record protocol_parse rejects: the canary for a
                             * driver frame-sync misframe (climbs if RX offset is wrong) */

    /* IMP-4 (NOT an error): late symbols for an already-emitted block, discarded
     * by the slot_for stale-bid guard rather than opened as a ghost slot. This is
     * the COMMON case under early-close -- roughly m per clean block (~9/anchor,
     * ~2/ref) -- so it is intentionally kept OUT of the drops_* error family. */
    uint64_t late_after_emit;
} FecRx;

void fec_rx_init(FecRx *f, FecRx_Emit emit, void *user);

/* Feed one outer record popped off the ring. now_ns drives the close timeout. */
void fec_rx_feed(FecRx *f, const uint8_t *record, size_t len, uint64_t now_ns);

/* Call once per loop iteration; flushes a stalled open block. */
void fec_rx_tick(FecRx *f, uint64_t now_ns);

#ifdef LATPROBE
void fec_rx_probe_dump(int fd);
#endif

#endif
