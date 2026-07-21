#ifndef TX_FEC_TX_H
#define TX_FEC_TX_H

#include <stdint.h>
#include <stddef.h>
#include "fec.h"
#include "protocol.h"   /* proto_mode_t, FEC_HDR_BYTES, proto_fec_pair_hdr_pack */
#include "fec_class.h"

/* Compile-time cap on the held-block ring. Production builds keep the historical
 * size 2 (the column pair) so the FecTx struct is byte-identical to today; the
 * offline block-interleaver study build overrides this (-DFEC_TX_RING_MAX=16) to
 * hold up to D=16 blocks for timing interleave. */
#ifndef FEC_TX_RING_MAX
#define FEC_TX_RING_MAX 2     /* production: the column pair only (current size) */
#endif

/* Push one fully-built outer packet ([flags][len][payload]) to the wire. */
typedef void (*FecTx_Push)(const uint8_t *packet, size_t len, void *user);

typedef struct {
    int      m_anchor;          /* parity for IDR/SPS/PPS class blocks */
    int      m_ref;             /* parity for ref-P class blocks (0 => plain) */
    int      m;                 /* parity of the CURRENTLY OPEN block (set per NAL) */
    int      block_group;       /* FecGroupKey of the open block; read only when ndata>0 */
    int      k_bound;           /* soft block close: flush at this many data syms (<=FEC_K_MAX) */
    int      block_ilv_depth;   /* 0/1 => OFF (exact current behavior); >=2 => block
                                 * (timing) interleave depth D: buffer D blocks and
                                 * emit their LEGACY packets round-robin by position
                                 * so a time-burst hits each block only ceil(L/D)
                                 * times. memset(0) in init leaves every existing
                                 * caller OFF and byte-identical. Set via
                                 * fec_tx_set_block_depth AFTER fec_tx_init. */
    proto_mode_t mode;          /* outer-packet framing mode (single per link) */
    uint8_t  block_id;          /* increments per emitted block, 7-bit, wraps at 128 */
    uint8_t *seq_counter;       /* shared with encoder: inner SEQ continuity */

    int      ndata;             /* data symbols buffered in the open block */
#ifdef LATPROBE
    uint64_t open_block_ingress_ns;   /* S2: birth of the currently-open block */
#endif
    uint8_t  data[FEC_K_MAX][FEC_SYMBOL_LEN];   /* zero-padded inner packets */

    /* Held full blocks awaiting an interleave partner (pairwise column emit).
     * A k_bound-full block is encoded into ready_sym[nready] and held; when two
     * equal-(k,n) full blocks are held they emit column-interleaved and clear.
     * Sized to the absolute max FEC_K_MAX+FEC_M_MAX even though a held block is
     * only ever <= k_bound+m rows (intentional safe headroom; never indexed
     * past n). */
    uint8_t  ready_sym[FEC_TX_RING_MAX][FEC_K_MAX + FEC_M_MAX][FEC_SYMBOL_LEN];
    uint8_t  ready_k[FEC_TX_RING_MAX], ready_n[FEC_TX_RING_MAX], ready_bid[FEC_TX_RING_MAX];
#ifdef LATPROBE
    uint64_t ready_ingress_ns[FEC_TX_RING_MAX];  /* S2: ingress carried into ready ring */
#endif
    int      nready;

    FecTx_Push push;
    void      *user;

    /* counters surfaced in the TX heartbeat */
    uint64_t  fec_blocks;
    uint64_t  fec_parity_pkts;
    uint64_t  oversize_drops;   /* NALs > 65535 B: u16 length prefix can't represent them -> dropped */
} FecTx;

void fec_tx_init(FecTx *f, int m_anchor, int m_ref, int k_bound,
                 uint8_t *seq_counter, proto_mode_t mode,
                 FecTx_Push push, void *user);

/* Enable the block (timing) interleaver at depth `depth` (clamped to
 * [0, FEC_TX_RING_MAX]). Call AFTER fec_tx_init. depth<2 leaves the exact current
 * (column/legacy) emission untouched; depth>=2 buffers D blocks and emits their
 * legacy packets round-robin (no new wire format, extra_flags=0). */
void fec_tx_set_block_depth(FecTx *f, int depth);

/* Feed one whole NAL (Annex-B start code already stripped; nal[0] is the NAL
 * header byte). Disposable NALs flush any open block then pass through. */
void fec_tx_nal(FecTx *f, const uint8_t *nal, size_t len);

/* Close + emit any open block (call at EOF / stream end). */
void fec_tx_flush(FecTx *f);

#ifdef LATPROBE
void fec_tx_probe_dump(int fd);   /* dump the S2 histogram to fd */
#endif

#endif
