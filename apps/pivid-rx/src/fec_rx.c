#define _POSIX_C_SOURCE 200809L   /* mono_ns.h -> clock_gettime/CLOCK_MONOTONIC (LATPROBE) */
#include "fec_rx.h"
#include "protocol.h"
#include <string.h>

#ifdef LATPROBE
#include "resid_hist.h"
#include "mono_ns.h"
static ResidHist g_s5_fec_close;
static const uint64_t S5_EDGES[] = {
    100000, 250000, 500000, 1000000, 2000000, 4000000,
    8000000, 16000000, 24000000, 33000000, (uint64_t)-1 };
void fec_rx_probe_dump(int fd) { resid_hist_dump(&g_s5_fec_close, fd); }
#endif

void fec_rx_init(FecRx *f, FecRx_Emit emit, void *user) {
    memset(f, 0, sizeof *f);
#ifdef LATPROBE
    resid_hist_init(&g_s5_fec_close, "s5_fec_close", S5_EDGES,
                    (int)(sizeof S5_EDGES / sizeof S5_EDGES[0]), 2000000ull);
#endif
    f->emit = emit;
    f->user = user;
}

/* Emit one data symbol's real inner record. The inner packet uses the FIXED
 * header (flags(1)+len(u16-LE,2)); its real record length is 3 + len, bounded
 * to one RS symbol. (A1: 2->3-byte FIXED header + u16-LE len read.) */
static void emit_data_symbol(FecRx *f, const uint8_t *sym) {
    size_t inner = (size_t)3 + (size_t)unpack_le16(&sym[1]);
    if (inner > FEC_SYMBOL_LEN) {        /* corrupt/misframe: a legit TX never does this */
        f->drops_inner_len++;
        return;                          /* reject, do not forward a doomed record */
    }
    f->emit(sym, inner, f->user);
}

/* a strictly older than b in 7-bit serial space (wrap at 128). */
static int serial_lt(uint8_t a, uint8_t b) {
    uint8_t d = (uint8_t)(b - a) & 0x7F;
    return d != 0 && d < 64;
}

/* IMP-4: all k data symbols present -> the block is deliverable NOW (slot_emit's
 * all-data fast path skips the RS decode). Subsumes have>=n: have==n means every
 * index is present, which includes 0..k-1. Under any DATA loss this is false, so
 * a lossy block is NOT early-closed -- it waits for parity/recovery exactly as
 * today (recovery path unchanged). */
static int slot_data_complete(const FecRxSlot *s) {
    for (int i = 0; i < s->k; i++) if (!s->present[i]) return 0;
    return 1;
}

/* Decode/recover one slot and EMIT its data symbols in idx order. */
static void slot_emit(FecRx *f, FecRxSlot *s) {
    int k = s->k, n = s->n;
    int all = slot_data_complete(s);   /* same all-k-data test used by the early-close gate */
    if (all) {
        for (int i = 0; i < k; i++) emit_data_symbol(f, s->sym[i]);
    } else {
        uint8_t *sp[FEC_K_MAX + FEC_M_MAX];
        for (int i = 0; i < n; i++) sp[i] = s->sym[i];
        if (fec_decode(sp, s->present, k, n) == 0) {
            f->fec_recovered++;
            for (int i = 0; i < k; i++) emit_data_symbol(f, s->sym[i]);
        } else {
            f->fec_unrecoverable++;
            /* Root-cause split (see fec_rx.h): did we even have k symbols? */
            if (s->have < k) { f->unrec_short++; f->unrec_deficit += (uint64_t)(k - s->have); }
            else             { f->unrec_present++; }   /* k present, decode failed => corrupt symbol */
            for (int i = 0; i < k; i++) if (s->present[i]) emit_data_symbol(f, s->sym[i]);
        }
    }
    f->fec_blocks++;
#ifdef LATPROBE
    /* slot_emit has no now_ns param -> sample the clock here (S5 egress). */
    if (s->first_sym_ns) {
        uint64_t now = pv_mono_ns();
        if (now >= s->first_sym_ns) resid_hist_add(&g_s5_fec_close, now - s->first_sym_ns);
    }
#endif
    f->last_emit_bid = s->block_id; f->have_last_emit = 1;
    s->used = 0; s->closed = 0;
}

/* Mark a slot closed (ready to flush) without emitting. */
static void slot_mark_closed(FecRxSlot *s) { s->closed = 1; }

/* Emit the serially-oldest closed slot, repeatedly, but only when NO older slot
 * is still open -- preserves ascending-block_id (inner-SEQ) order. */
static void drain_ordered(FecRx *f) {
    for (;;) {
        int oldest = -1;
        for (int i = 0; i < FEC_RX_SLOTS; i++) {
            if (!f->slot[i].used) continue;
            if (oldest < 0 || serial_lt(f->slot[i].block_id, f->slot[oldest].block_id)) oldest = i;
        }
        if (oldest < 0) break;
        if (!f->slot[oldest].closed) break;     /* oldest not ready -> wait (ordered) */
        slot_emit(f, &f->slot[oldest]);
    }
}

/* Find the slot holding block_id, or open a free one, or evict+flush the oldest. */
static FecRxSlot *slot_for(FecRx *f, uint8_t bid, int k, int n, uint64_t now_ns) {
    for (int i = 0; i < FEC_RX_SLOTS; i++)
        if (f->slot[i].used && f->slot[i].block_id == bid) return &f->slot[i];
    /* IMP-4 ghost-slot guard: a bid not serially newer than the last emitted
     * block was already flushed (emit frees its slot). Refuse to OPEN a new slot
     * for it -- its late parity is discarded by the caller. An EXISTING live slot
     * for this bid was returned above (a live slot is never stale), so this gates
     * only new opens, and a stale bid can never evict a live block. */
    if (f->have_last_emit && !serial_lt(f->last_emit_bid, bid)) return NULL;
    int free_i = -1, oldest = -1;
    for (int i = 0; i < FEC_RX_SLOTS; i++) {
        if (!f->slot[i].used) { free_i = i; break; }
        if (oldest < 0 || serial_lt(f->slot[i].block_id, f->slot[oldest].block_id)) oldest = i;
    }
    if (free_i < 0) {            /* all full: force-close + emit the oldest (it IS oldest) */
        slot_mark_closed(&f->slot[oldest]);
        slot_emit(f, &f->slot[oldest]);
        free_i = oldest;
    }
    FecRxSlot *s = &f->slot[free_i];
    memset(s, 0, sizeof *s);
    s->used = 1; s->block_id = bid; s->k = k; s->n = n; s->last_progress_ns = now_ns;
#ifdef LATPROBE
    s->first_sym_ns = now_ns;
#endif
    if (f->last_open_bid_valid) {
        uint8_t step = (uint8_t)(bid - f->last_open_bid);
        if (step == 0 || step > 8) f->bid_noninc++;
    }
    f->last_open_bid = bid; f->last_open_bid_valid = 1;
    return s;
}

/* Walk the `count` on-wire symbols, filling off[]/len[] with each symbol's byte
 * offset and length. A DATA symbol (block index < k) is self-describing: its
 * length is 3 + the inner FIXED len field at sym[1..2] (the TX dropped the
 * zero-pad past it). A PARITY symbol (index >= k) is full FEC_SYMBOL_LEN. Every
 * read is bounded by `plen` BEFORE access (no OOB); each data length is capped at
 * FEC_SYMBOL_LEN (no zero-extend overflow); the exact `o == plen` total is the
 * misframe / FIXED-vs-DYNAMIC mismatch guard that replaced the fixed-stride `==`.
 * Returns 0 on success, -1 on any bound/total violation (caller -> drops_len). */
static int fec_walk_lengths(const uint8_t *payload, size_t plen, uint8_t count,
                            uint8_t idx0, uint8_t idx1, uint8_t k,
                            size_t off[2], size_t len[2]) {
    uint8_t idx[2] = { idx0, idx1 };
    size_t o = FEC_HDR_BYTES;
    for (uint8_t t = 0; t < count; t++) {
        size_t sl;
        if (idx[t] < k) {                                  /* data: self-describing */
            if (o + 3 > plen) return -1;                   /* inner header in-bounds */
            sl = (size_t)3 + (size_t)unpack_le16(&payload[o + 1]);
            if (sl > FEC_SYMBOL_LEN) return -1;            /* zero-extend target bound */
        } else {
            sl = FEC_SYMBOL_LEN;                           /* parity: full width */
        }
        if (o + sl > plen) return -1;                      /* symbol bytes in-bounds */
        off[t] = o; len[t] = sl; o += sl;
    }
    return (o == plen) ? 0 : -1;                           /* strict: no trailing pad */
}

void fec_rx_feed(FecRx *f, const uint8_t *record, size_t len, uint64_t now_ns) {
    uint8_t flags, payload[PROTOCOL_MAX_PAYLOAD]; size_t plen;
    if (protocol_parse(record, len, PROTO_MODE_FIXED, &flags, payload, &plen) != 0) { f->drops_parse++; return; }

    if (!(flags & PROTO_FLAG_FEC)) {
        /* Disposable / unprotected: flush all open blocks first (they precede any
         * disposable on the wire), then pass the record straight to reasm. */
        for (int i = 0; i < FEC_RX_SLOTS; i++) if (f->slot[i].used) slot_mark_closed(&f->slot[i]);
        drain_ordered(f);
        f->emit(record, len, f->user);
        return;
    }

    if (plen < FEC_HDR_BYTES) { f->drops_malformed++; return; }  /* malformed */
    uint8_t bid, base_idx, k, n, count;
    proto_fec_pair_hdr_unpack(payload, &bid, &base_idx, &k, &n, &count);

    /* (a) Pair-header sanity: count in {1,2}, 1<=k<=n, n<=k+FEC_M_MAX,
     * base_idx+count<=n. Drop+count on any violation. The (int) casts are
     * load-bearing: they force the sums into int so base_idx=254 + count=2
     * = 256 does NOT wrap a u8 -- never fold these into a u8 intermediate. */
    if (count < 1 || count > 2) { f->drops_sanity++; return; }
    if (k < 1 || k > n || n > (int)k + FEC_M_MAX) { f->drops_sanity++; return; }
    /* Absolute array bound: a forged/misframed k or n must never index past the
     * per-slot sym[FEC_K_MAX+FEC_M_MAX] array (nor exceed fec_decode's limits).
     * The strict-length check below bounds only the PAYLOAD size, not the header's
     * k/n; without this, e.g. k=223,n=255 passes n<=k+M and then writes sym[254]
     * (OOB; arrays hold only FEC_K_MAX+FEC_M_MAX=232 rows). Legit prod blocks have
     * k<=k_bound(16), n<=k+m_anchor(9)=25 -- well under these caps. */
    if (k > FEC_K_MAX || n > FEC_K_MAX + FEC_M_MAX) { f->drops_sanity++; return; }
    /* The base_idx+count<=n run-bound applies ONLY to legacy (consecutive)
     * packets: there base_idx..base_idx+count-1 are real indices in ONE block.
     * In an ILV packet base_idx is a single shared COLUMN j (count=2 symbols go
     * to the SAME index j in TWO blocks), so the bound is `col < n` instead --
     * enforced inside the ILV branch below. */
    if (!(flags & PROTO_FLAG_FEC_ILV) && (int)base_idx + (int)count > n) { f->drops_sanity++; return; }

    /* (b) Length walk (OOB + misframe guard). Data symbols are truncated to their
     * inner length on the wire; parity stays full FEC_SYMBOL_LEN. The walk bounds
     * every read by `plen` and requires the symbol lengths to sum EXACTLY to plen
     * -- the self-describing replacement for the old fixed-stride `==`. idx0/idx1
     * are the two on-wire symbols' block indices: consecutive for legacy, the SAME
     * shared column for ILV (both checked against k -> same data/parity verdict). */
    uint8_t walk_idx0 = base_idx;
    uint8_t walk_idx1 = (flags & PROTO_FLAG_FEC_ILV) ? base_idx : (uint8_t)(base_idx + 1);
    size_t sym_off[2], sym_len[2];
    if (fec_walk_lengths(payload, plen, count, walk_idx0, walk_idx1, k, sym_off, sym_len) != 0) {
        f->drops_len++; return;
    }

    if (flags & PROTO_FLAG_FEC_ILV) {
        /* Interleaved column packet: id field = block_A, base_idx = column j.
         * count is always 2; symbol 0 -> block_A[j], symbol 1 -> block_B[j],
         * with block_B = (block_A + 1) & 0x7F (two DIFFERENT adjacent blocks at
         * the SAME index j). Route each into its own slot. */
        if (count != 2) { f->drops_sanity++; return; }
        uint8_t col = base_idx;
        if (col >= n) { f->drops_sanity++; return; }
        uint8_t bidA = bid, bidB = (uint8_t)((bid + 1u) & 0x7F);
        FecRxSlot *sa = slot_for(f, bidA, k, n, now_ns);
        FecRxSlot *sb = slot_for(f, bidB, k, n, now_ns);
        if (!sa || !sb) { f->late_after_emit++; return; }   /* IMP-4: stale block(s) -> discard */
        /* Eviction-collapse guard. slot_for evicts the GLOBAL serially-oldest
         * slot. Under forged/misframed ids the 4 slots can all hold ids newer
         * than bidA; then sa(bidA) is itself the oldest, and slot_for(bidB)
         * evicts sa's own slot and re-targets it to bidB -> sa==sb. The k/n
         * recheck cannot catch this (slot_for just set k,n on the collapsed
         * slot), so detect the aliasing directly and drop LOUDLY before any
         * write -- otherwise the two symbol stores would alias and have++ would
         * double-count, silently, during exactly a misframe storm. */
        if (sa == sb) { f->drops_sanity++; return; }
        if (sa->k != k || sa->n != n || sb->k != k || sb->n != n) { f->drops_sanity++; return; }
        const uint8_t *srcA = payload + sym_off[0];
        const uint8_t *srcB = payload + sym_off[1];
        if (!sa->present[col]) { memset(sa->sym[col], 0, FEC_SYMBOL_LEN); memcpy(sa->sym[col], srcA, sym_len[0]); sa->present[col] = 1; sa->have++; }
        if (!sb->present[col]) { memset(sb->sym[col], 0, FEC_SYMBOL_LEN); memcpy(sb->sym[col], srcB, sym_len[1]); sb->present[col] = 1; sb->have++; }
        sa->last_progress_ns = now_ns; sb->last_progress_ns = now_ns;
        if (slot_data_complete(sa)) slot_mark_closed(sa);
        if (slot_data_complete(sb)) slot_mark_closed(sb);
        drain_ordered(f);
        return;
    }

    FecRxSlot *s = slot_for(f, bid, k, n, now_ns);
    if (!s) { f->late_after_emit++; return; }   /* IMP-4: stale bid, late symbol -> discard */
    /* Defensive / forward-looking: a slot re-resolved for this bid must agree on
     * (k,n) with the symbols already in it; a mismatch is a misframe/forged
     * sub-header. For legacy traffic this never fires (a bid carries one (k,n));
     * it becomes load-bearing once interleave lets two blocks share a bid. */
    if (s->k != k || s->n != n) { f->drops_sanity++; return; }

    /* Demux `count` consecutive on-wire symbols at base_idx, base_idx+1. Each is
     * truncated to its self-describing inner length (data) or full FEC_SYMBOL_LEN
     * (parity); the walk above filled sym_off[]/sym_len[] and bounds-checked them. */
    for (uint8_t t = 0; t < count; t++) {
        uint8_t idx = (uint8_t)(base_idx + t);
        if (idx >= n) break;                          /* defensive (sanity bounds base+count<=n) */
        const uint8_t *src = payload + sym_off[t];
        if (!s->present[idx]) {
            memset(s->sym[idx], 0, FEC_SYMBOL_LEN);   /* zero-extend truncated tail */
            memcpy(s->sym[idx], src, sym_len[t]);
            s->present[idx] = 1;
            s->have++;
        }
    }
    s->last_progress_ns = now_ns;

    if (slot_data_complete(s)) slot_mark_closed(s);   /* IMP-4: all k data present -> deliver now */
    drain_ordered(f);
}

void fec_rx_tick(FecRx *f, uint64_t now_ns) {
    for (int i = 0; i < FEC_RX_SLOTS; i++)
        if (f->slot[i].used && !f->slot[i].closed &&
            now_ns - f->slot[i].last_progress_ns >= FEC_RX_TIMEOUT_NS)
            slot_mark_closed(&f->slot[i]);
    drain_ordered(f);
}
