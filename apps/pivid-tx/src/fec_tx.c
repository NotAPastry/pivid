#define _POSIX_C_SOURCE 200809L   /* mono_ns.h -> clock_gettime/CLOCK_MONOTONIC (LATPROBE) */
#include "fec_tx.h"
#ifdef LATPROBE
#include "resid_hist.h"
#include "mono_ns.h"
static ResidHist g_s2_fec_tx;
static const uint64_t S2_EDGES[] = {
    500000, 1000000, 2000000, 4000000, 8000000, 16000000,
    24000000, 32000000, 40000000, 50000000, 70000000, (uint64_t)-1 };
static void s2_measure_from(uint64_t t0) {
    if (t0) { uint64_t now = pv_mono_ns(); if (now >= t0) resid_hist_add(&g_s2_fec_tx, now - t0); }
}
static void s2_measure_slot(const FecTx *f, int slot) { s2_measure_from(f->ready_ingress_ns[slot]); }
void fec_tx_probe_dump(int fd) { resid_hist_dump(&g_s2_fec_tx, fd); }
#endif
#include "protocol.h"
#include "fec_class.h"
#include <assert.h>
#include <string.h>

/* Protected-NAL fragmentation cap: inner packet must fit one RS symbol.
 * inner is built in FIXED mode = [flags(1)][len u16][payload]; the 3-byte
 * header leaves payload <= S - 3. (A1 widened the FIXED header to 3 bytes.) */
#define FEC_INNER_PAYLOAD_MAX (FEC_SYMBOL_LEN - 3)   /* 249 */

/* Plain (FEC-off) fragmentation cap = the protocol ceiling. Production runs
 * m>=1 (FEC always on), so this path is non-production; the historical 252 B
 * byte-254 mitigation was a 255 B FLRC artifact and no longer applies. */
#define PLAIN_CHUNK_MAX PROTOCOL_MAX_PAYLOAD

void fec_tx_init(FecTx *f, int m_anchor, int m_ref, int k_bound,
                 uint8_t *seq_counter, proto_mode_t mode,
                 FecTx_Push push, void *user) {
    memset(f, 0, sizeof *f);
#ifdef LATPROBE
    resid_hist_init(&g_s2_fec_tx, "s2_fec_tx", S2_EDGES,
                    (int)(sizeof S2_EDGES / sizeof S2_EDGES[0]), 5000000ull);
#endif
    if (m_anchor < 0) m_anchor = 0;
    if (m_anchor > FEC_M_MAX) m_anchor = FEC_M_MAX;
    if (m_ref < 0) m_ref = 0;
    if (m_ref > FEC_M_MAX) m_ref = FEC_M_MAX;
    f->m_anchor = m_anchor;
    f->m_ref = m_ref;
    if (k_bound < 1) k_bound = 1;
    if (k_bound > FEC_K_MAX) k_bound = FEC_K_MAX;
    f->k_bound = k_bound;
    f->seq_counter = seq_counter;
    f->mode = mode;
    f->push = push;
    f->user = user;
}

void fec_tx_set_block_depth(FecTx *f, int depth) {
    if (depth < 0) depth = 0;
    if (depth > FEC_TX_RING_MAX) depth = FEC_TX_RING_MAX;
    f->block_ilv_depth = depth;
}

/* Build + push plain (FEC=0) outer packets for a disposable (or m==0) NAL.
 * A multi-fragment NAL's first packet carries a 2-byte LE total-length prefix
 * (START && !END); single-packet NALs are atomic and carry none. */
static void push_plain(FecTx *f, const uint8_t *buf, size_t n) {
    assert(n <= 0xFFFFu);                 /* length prefix is u16; NALs are < 64 KB */
    int multi = (n > PLAIN_CHUNK_MAX);
    size_t off = 0;
    int first = 1;
    do {
        uint8_t pl[2 + PROTOCOL_MAX_PAYLOAD];
        size_t hdr = 0, cap = PLAIN_CHUNK_MAX;
        if (first && multi) { pack_le16(pl, (uint16_t)n); hdr = 2; cap -= 2; }
        size_t take = (n - off) > cap ? cap : (n - off);
        memcpy(pl + hdr, buf + off, take);

        uint8_t flags = PROTO_FLAG_SEQ(*f->seq_counter);
        if (first)              flags |= PROTO_FLAG_START;
        if (off + take >= n)    flags |= PROTO_FLAG_END;
        /* Invariant: video flags carry TYPE=0 (bit 0 clear). */
        assert((flags & FLAG_TYPE_MASK) == 0);

        uint8_t pkt[3 + 2 + PROTOCOL_MAX_PAYLOAD];
        int len = protocol_build(pkt, sizeof pkt, f->mode, flags, pl, hdr + take);
        if (len > 0) f->push(pkt, (size_t)len, f->user);
        *f->seq_counter = (uint8_t)((*f->seq_counter + 1u) & 0x0F);

        off += take;
        first = 0;
    } while (off < n);
}

/* On-wire length of one coded symbol at block index `idx`. A data symbol
 * (idx < k) ships only its real inner FIXED packet (3 + the len field at
 * sym[1..2]), dropping the zero-pad; a parity symbol (idx >= k) ships full
 * width. Mirror of the RX's per-symbol rule in fec_walk_lengths(). */
static size_t fec_sym_wire_len(const uint8_t *sym, uint8_t idx, uint8_t k) {
    if (idx < k) return (size_t)3 + (size_t)unpack_le16(&sym[1]);
    return FEC_SYMBOL_LEN;
}

/* Emit one packet carrying 1 or 2 symbols with an explicit id field + flags.
 * For legacy: extra_flags=0, id=block_id, base=base_idx. For interleave:
 * extra_flags=PROTO_FLAG_FEC_ILV, id=block_A, base=column j. `s0`/`s1` are the
 * in-memory full FEC_SYMBOL_LEN coded symbols, but only their on-wire length is
 * copied: a DATA symbol (block index < k) ships truncated to its real inner
 * packet (3 + LE16(sym[1]), the zero-pad dropped); PARITY (index >= k) ships
 * full width. id masked to 7 bits; the pair header carries no mode bit (a TX/RX
 * mode mismatch is caught by the RX sanity check + the fec_walk_lengths length
 * walk, which sums the self-describing symbol lengths to the exact payload). */
static void emit_pair(FecTx *f, uint8_t extra_flags, uint8_t id, uint8_t base,
                      uint8_t k, uint8_t n, const uint8_t *s0, const uint8_t *s1) {
    uint8_t payload[FEC_HDR_BYTES + 2 * FEC_SYMBOL_LEN];
    proto_fec_pair_hdr_pack(payload, (uint8_t)(id & 0x7F), base, k, n, s1 ? 2u : 1u);
    int ilv = (extra_flags & PROTO_FLAG_FEC_ILV) != 0;
    size_t l0 = fec_sym_wire_len(s0, base, k);
    memcpy(payload + FEC_HDR_BYTES, s0, l0);
    size_t plen = (size_t)FEC_HDR_BYTES + l0;
    if (s1) {
        uint8_t idx1 = ilv ? base : (uint8_t)(base + 1);
        size_t l1 = fec_sym_wire_len(s1, idx1, k);
        memcpy(payload + plen, s1, l1);
        plen += l1;
    }
    uint8_t pkt[3 + FEC_HDR_BYTES + 2 * FEC_SYMBOL_LEN];
    uint8_t flags = (uint8_t)(PROTO_FLAG_FEC | extra_flags);
    /* Invariant: video flags carry TYPE=0 (bit 0 clear). The TYPE bit is owned
     * by the telemetry path; never set bit 0 here. */
    assert((flags & FLAG_TYPE_MASK) == 0);
    int len = protocol_build(pkt, sizeof pkt, f->mode, flags, payload, plen);
    if (len > 0) f->push(pkt, (size_t)len, f->user);
}

/* RS-encode the open block into dst[0..n-1]; returns n, sets *out_k. dst rows are
 * full FEC_SYMBOL_LEN coded symbols (data copied, parity appended). */
static int encode_block_into(FecTx *f, uint8_t dst[][FEC_SYMBOL_LEN], int *out_k) {
    int k = f->ndata, m = f->m, n = k + m;
    for (int i = 0; i < k; i++) memcpy(dst[i], f->data[i], FEC_SYMBOL_LEN);
    if (m > 0) {
        uint8_t parity[FEC_M_MAX][FEC_SYMBOL_LEN];
        const uint8_t *dp[FEC_K_MAX]; uint8_t *pp[FEC_M_MAX];
        for (int i = 0; i < k; i++) dp[i] = f->data[i];
        for (int i = 0; i < m; i++) pp[i] = parity[i];
        fec_encode(dp, k, m, pp);
        for (int i = 0; i < m; i++) { memcpy(dst[k + i], parity[i], FEC_SYMBOL_LEN); f->fec_parity_pkts++; }
    }
    *out_k = k;
    return n;
}

/* Emit one held block legacy (consecutive pairs + count=1 tail). */
static void emit_block_legacy(FecTx *f, uint8_t dst[][FEC_SYMBOL_LEN],
                              uint8_t bid, int k, int n) {
    (void)k;
    int i = 0;
    while (i + 1 < n) { emit_pair(f, 0, bid, (uint8_t)i, (uint8_t)k, (uint8_t)n, dst[i], dst[i + 1]); i += 2; }
    if (i < n)        emit_pair(f, 0, bid, (uint8_t)i, (uint8_t)k, (uint8_t)n, dst[i], NULL);
}

/* Block (timing) interleaver: emit the first `count` held ring blocks' LEGACY
 * packets round-robin by packet position. For position r (covering symbols 2r,
 * 2r+1): for each held block b, if block b still has a packet at position r
 * (2r < n_b), emit it via the existing legacy emit_pair (extra_flags=0). A
 * time-burst of L consecutive emitted packets therefore touches each block only
 * ceil(L/count) times. The multiset of emitted packets is exactly the legacy
 * emission of each held block -- only the temporal order changes (airtime- and
 * parity-neutral). Held order is preserved at each position so following
 * plain/disposable NALs still come after these protected blocks. After emit the
 * caller clears nready. */
static void emit_ring_block_interleaved(FecTx *f, int count) {
#ifdef LATPROBE
    for (int b = 0; b < count; b++) s2_measure_slot(f, b);   /* count slots this emitter drains */
#endif
    /* widest block bounds the position loop; uneven n just skips exhausted blocks */
    int max_n = 0;
    for (int b = 0; b < count; b++) if (f->ready_n[b] > max_n) max_n = f->ready_n[b];
    for (int base = 0; base < max_n; base += 2) {
        for (int b = 0; b < count; b++) {
            int n = f->ready_n[b], k = f->ready_k[b];
            if (base >= n) continue;
            const uint8_t *s0 = f->ready_sym[b][base];
            const uint8_t *s1 = (base + 1 < n) ? f->ready_sym[b][base + 1] : NULL;
            emit_pair(f, 0, f->ready_bid[b], (uint8_t)base, (uint8_t)k, (uint8_t)n, s0, s1);
        }
    }
}

/* Emit two equal-(k,n) blocks column-interleaved: packet j = (A[j], B[j]). */
#ifndef FEC_FORCE_LEGACY   /* unused (compiled out) in the host fecsim_legacy build */
static void emit_pair_interleaved(FecTx *f, int idxA, int idxB) {
#ifdef LATPROBE
    s2_measure_slot(f, idxA); s2_measure_slot(f, idxB);
#endif
    int k = f->ready_k[idxA], n = f->ready_n[idxA];
    uint8_t bidA = f->ready_bid[idxA];
    for (int j = 0; j < n; j++)
        emit_pair(f, PROTO_FLAG_FEC_ILV, bidA, (uint8_t)j, (uint8_t)k, (uint8_t)n,
                  f->ready_sym[idxA][j], f->ready_sym[idxB][j]);
}
#endif /* !FEC_FORCE_LEGACY */

/* Soft close at k_bound: encode the full block and hold it for interleaving.
 * When two equal-(k,n) blocks are held, emit them column-interleaved. */
static void block_soft_close(FecTx *f) {
    if (f->ndata == 0) return;
    int slot = f->nready;
    int k, n = encode_block_into(f, f->ready_sym[slot], &k);
    f->ready_k[slot] = (uint8_t)k; f->ready_n[slot] = (uint8_t)n; f->ready_bid[slot] = f->block_id;
#ifdef LATPROBE
    f->ready_ingress_ns[slot] = f->open_block_ingress_ns;
#endif
    f->nready++;
    f->fec_blocks++;
    f->block_id = (uint8_t)((f->block_id + 1u) & 0x7F);
    f->ndata = 0;
    if (f->block_ilv_depth >= 2) {
        /* Block (timing) interleave: hold D blocks, then emit them round-robin.
         * Mutually exclusive with the column pair below. */
        if (f->nready == f->block_ilv_depth) {
            emit_ring_block_interleaved(f, f->nready);
            f->nready = 0;
        }
        return;
    }
    if (f->nready == 2) {
#ifndef FEC_FORCE_LEGACY
        if (f->ready_k[0] == f->ready_k[1] && f->ready_n[0] == f->ready_n[1]) {
            emit_pair_interleaved(f, 0, 1);
        } else {
            /* Unequal (k,n) -> can't column-interleave; emit both legacy.
             * (Defensive: equal k_bound + constant per-group m make dims always
             * match today, so a held pair only ever differs if k_bound or m
             * changes mid-block-group -- not a path the current feeder takes.) */
#ifdef LATPROBE
            s2_measure_slot(f, 0); s2_measure_slot(f, 1);   /* emit_block_legacy has no slot arg -> measure here */
#endif
            emit_block_legacy(f, f->ready_sym[0], f->ready_bid[0], f->ready_k[0], f->ready_n[0]);
            emit_block_legacy(f, f->ready_sym[1], f->ready_bid[1], f->ready_k[1], f->ready_n[1]);
        }
#else
        /* FEC_FORCE_LEGACY (host fecsim_legacy build only): never column-interleave;
         * emit both held blocks via the SAME real production legacy emit path used
         * for the unequal-dims case above. This is the sweep's ilv=off arm. The
         * production binary (this macro undefined) is byte-for-byte unchanged.
         * Flush-time singletons (fec_tx_flush, nready==1) already emit legacy
         * unconditionally, so a lone tail block is never dropped under this guard. */
        emit_block_legacy(f, f->ready_sym[0], f->ready_bid[0], f->ready_k[0], f->ready_n[0]);
        emit_block_legacy(f, f->ready_sym[1], f->ready_bid[1], f->ready_k[1], f->ready_n[1]);
#endif
        f->nready = 0;
    }
}

/* Append one NAL's data symbols to the open block. Each data symbol is an inner
 * FIXED protocol packet (START/END/SEQ) zero-padded to S. A multi-fragment NAL's
 * first symbol carries the 2-byte LE total-length prefix (START && !END). */
static void block_append_nal(FecTx *f, const uint8_t *nal, size_t len,
                             FecNalClass klass) {
    assert(len <= 0xFFFFu);               /* length prefix is u16; NALs are < 64 KB */
    int multi = (len > FEC_INNER_PAYLOAD_MAX);
    size_t off = 0;
    int first = 1;
    do {
        if (f->ndata >= f->k_bound) { block_soft_close(f); }   /* hold for interleave */

        uint8_t pl[2 + FEC_INNER_PAYLOAD_MAX];
        size_t hdr = 0, cap = FEC_INNER_PAYLOAD_MAX;
        if (first && multi) { pack_le16(pl, (uint16_t)len); hdr = 2; cap -= 2; }
        size_t take = (len - off) > cap ? cap : (len - off);
        memcpy(pl + hdr, nal + off, take);

        uint8_t flags = PROTO_FLAG_SEQ(*f->seq_counter);
        if (first)             flags |= PROTO_FLAG_START;
        if (off + take >= len) flags |= PROTO_FLAG_END;
        /* Invariant: video flags carry TYPE=0 (bit 0 clear). */
        assert((flags & FLAG_TYPE_MASK) == 0);
        *f->seq_counter = (uint8_t)((*f->seq_counter + 1u) & 0x0F);

        uint8_t *sym = f->data[f->ndata];
        memset(sym, 0, FEC_SYMBOL_LEN);                    /* virtual zero-pad */
        int inner = protocol_build(sym, FEC_SYMBOL_LEN, PROTO_MODE_FIXED,
                                   flags, pl, hdr + take);
        if (inner > 0) {
            if (f->ndata == 0) {
                f->block_group = (int)fec_group_key(klass);
#ifdef LATPROBE
                f->open_block_ingress_ns = pv_mono_ns();
#endif
            }
            f->ndata++;
        }
        off += take;
        first = 0;
    } while (off < len);
    /* Invariant: every NAL appends >=1 data symbol to the open block. A held
     * (nready) block therefore never spans a NAL/group boundary unflushed --
     * the open block always carries this NAL's tail until the next soft-close
     * or fec_tx_flush. */
    assert(f->ndata >= 1);
}

void fec_tx_flush(FecTx *f) {
    if (f->block_ilv_depth >= 2) {
        /* Block-interleave flush: fold the open partial tail block into the held
         * ring (as one more held block) and emit the WHOLE ring round-robin in a
         * single pass. The tail is a protected block of the same group, so letting
         * it join the round-robin (a) spreads a burst across it too -- the win for
         * the common slice case where a group yields only 1-2 blocks before its
         * boundary flush -- and (b) still keeps every protected block ahead of any
         * following plain/disposable NAL (the whole ring emits before we return).
         *
         * The tail ALWAYS fits: block_soft_close emits + resets nready=0 the moment
         * nready==block_ilv_depth, and block_ilv_depth <= FEC_TX_RING_MAX (clamped
         * in fec_tx_set_block_depth), so on entry here nready is in
         * [0, block_ilv_depth-1] <= FEC_TX_RING_MAX-1 -- a free slot always exists.
         * The assert pins that invariant so a future change to the depth/
         * FEC_TX_RING_MAX relationship can't silently overflow ready_*. */
        assert(f->nready < FEC_TX_RING_MAX);
        if (f->ndata > 0) {
            int slot = f->nready;
            int k, n = encode_block_into(f, f->ready_sym[slot], &k);
            f->ready_k[slot] = (uint8_t)k; f->ready_n[slot] = (uint8_t)n;
            f->ready_bid[slot] = f->block_id;
#ifdef LATPROBE
            f->ready_ingress_ns[slot] = f->open_block_ingress_ns;
#endif
            f->nready++;
            f->fec_blocks++;
            f->block_id = (uint8_t)((f->block_id + 1u) & 0x7F);
            f->ndata = 0;
        }
        if (f->nready > 0) {
            emit_ring_block_interleaved(f, f->nready);
            f->nready = 0;
        }
        return;
    }
    /* Close the open (final/partial) block of the NAL into a temp buffer. */
    if (f->ndata > 0) {
        static uint8_t tail[FEC_K_MAX + FEC_M_MAX][FEC_SYMBOL_LEN];
        int k, n = encode_block_into(f, tail, &k);
        uint8_t bid = f->block_id;
        f->fec_blocks++;
        f->block_id = (uint8_t)((f->block_id + 1u) & 0x7F);
        f->ndata = 0;
        if (f->nready == 1) {
#ifdef LATPROBE
            s2_measure_slot(f, 0);
#endif
            emit_block_legacy(f, f->ready_sym[0], f->ready_bid[0], f->ready_k[0], f->ready_n[0]);
            f->nready = 0;
        }
#ifdef LATPROBE
        s2_measure_from(f->open_block_ingress_ns);   /* tail: emitted straight from open block, never in ready ring */
#endif
        emit_block_legacy(f, tail, bid, k, n);
    } else if (f->nready == 1) {
#ifdef LATPROBE
        s2_measure_slot(f, 0);
#endif
        emit_block_legacy(f, f->ready_sym[0], f->ready_bid[0], f->ready_k[0], f->ready_n[0]);
        f->nready = 0;
    }
}

void fec_tx_nal(FecTx *f, const uint8_t *nal, size_t len) {
    if (len == 0) return;
    if (len > 0xFFFFu) { f->oversize_drops++; return; }  /* u16 length prefix can't represent it; drop, don't abort */
    FecNalClass c = fec_nal_classify(nal[0]);
    int m = fec_class_m(c, f->m_anchor, f->m_ref);
    if (m <= 0) {
        /* Passthrough class (ref-P at m_ref=0, or disposables): flush any open
         * block first so wire order is [anchor block][following plain], then
         * send the NAL plain. */
        fec_tx_flush(f);
        push_plain(f, nal, len);
        return;
    }
    /* Grouped blocks: a NAL whose group key differs from the open block flushes
     * it first. SPS+PPS share GRP_PARAMSET; IDR/REF stay their own groups.
     * (block_group is only read when ndata>0, so a just-flushed empty block does
     * not trigger a spurious second flush.) */
    if (f->ndata > 0 && (int)fec_group_key(c) != f->block_group) fec_tx_flush(f);
    f->m = m;
    block_append_nal(f, nal, len, c);
}
