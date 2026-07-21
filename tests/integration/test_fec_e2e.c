#include "../apps/pivid-tx/src/fec_tx.h"
#include "../apps/pivid-rx/src/fec_rx.h"
#include "../apps/pivid-rx/src/reasm.h"
#include "protocol.h"
#include "fec.h"
#include <stdio.h>
#include <string.h>

static int g_failures=0;
#define EXPECT(c,m) do{ if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,m);g_failures++;} }while(0)

/* Captured wire packets from the TX stage. */
#define WCAP 512
static uint8_t g_wire[WCAP][512]; static size_t g_wlen[WCAP]; static int g_wn;
static void tx_capture(const uint8_t *p,size_t n,void *u){ (void)u; if(g_wn<WCAP){memcpy(g_wire[g_wn],p,n);g_wlen[g_wn]=n;g_wn++;} }

/* Reassembled NALs out of reasm. */
#define NCAP 48
static uint8_t g_nal[NCAP][REASM_BUF_BYTES]; static size_t g_nlen[NCAP]; static int g_nn;
static void nal_capture(const uint8_t *p,size_t n,void *u){ (void)u; if(g_nn<NCAP){memcpy(g_nal[g_nn],p,n);g_nlen[g_nn]=n;g_nn++;} }
static void fec_to_reasm(const uint8_t *r,size_t n,void *u){ reasm_feed((Reasm*)u,r,n); }

/* Run one NAL through TX(m) -> drop wire packets in `drop[]` -> RX -> reasm. */
static void run(int m, const uint8_t *nal, size_t nlen,
                const int *drop, int ndrop) {
    g_wn = 0; g_nn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, m, m, FEC_K_MAX, &seq, PROTO_MODE_FIXED, tx_capture, NULL);
    fec_tx_nal(&tx, nal, nlen);
    fec_tx_flush(&tx);

    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);
    for (int i = 0; i < g_wn; i++) {
        int dropped = 0;
        for (int d = 0; d < ndrop; d++) if (drop[d] == i) dropped = 1;
        if (!dropped) fec_rx_feed(&rx, g_wire[i], g_wlen[i], (uint64_t)(i+1)*1000);
    }
    fec_rx_tick(&rx, 0xFFFFFFFFFFFFFFFFull);   /* flush any open block */
}

static void test_anchor_survives_one_loss(void){
    uint8_t nal[600]; nal[0]=0x61; for(int i=1;i<600;i++) nal[i]=(uint8_t)(i*3);
    /* 600B -> ceil(600/246)=3 data syms; m=2 -> n=5 -> 3 wire packets:
     * pairs (0,1),(2,3) + tail (4). Wire idx 1 = the second pair (syms 2,3)
     * = 2 losses; present=3 >= k=3 so RS still recovers. */
    int drop[1]={1};
    run(2, nal, sizeof nal, drop, 1);
    EXPECT(g_nn==1, "anchor survives a packed-pair loss");
    EXPECT(g_nlen[0]==sizeof nal && memcmp(g_nal[0],nal,sizeof nal)==0, "byte-identical recovery");
}

static void test_anchor_drops_beyond_m(void){
    uint8_t nal[600]; nal[0]=0x61; for(int i=1;i<600;i++) nal[i]=(uint8_t)(i*3);
    /* m=1 but drop 2 data packets -> unrecoverable -> reasm drops the NAL (today's behaviour). */
    int drop[2]={0,1};
    run(1, nal, sizeof nal, drop, 2);
    EXPECT(g_nn==0, "beyond-m loss degrades to drop (no smear, no crash)");
}

static void test_disposable_unprotected(void){
    uint8_t nal[300]; nal[0]=0x01; for(int i=1;i<300;i++) nal[i]=(uint8_t)i;
    /* No drops: disposable passes through and reassembles. */
    run(2, nal, sizeof nal, NULL, 0);
    EXPECT(g_nn==1 && g_nlen[0]==sizeof nal, "disposable NAL passes through intact");
}

/* Replicate the LIVE scenario: a long multi-block stream of consecutive anchor
 * NALs, with a deterministic every-20th-packet drop (matching the RX debug knob
 * PIVID_RX_DROP_PCT=5 fractional accumulator). With m=2 over k<=16 blocks, each
 * 18-packet block loses 0-1 packets -> all must recover byte-identically and the
 * RX must NOT fabricate extra blocks (one close per TX block). */
static void test_multiblock_stream_periodic_loss(void){
    g_wn = 0; g_nn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, 2, 2, 16, &seq, PROTO_MODE_FIXED, tx_capture, NULL);   /* anchor+ref m=2, K=16 */
    /* 40 anchor NALs of 600 bytes -> 3 symbols each -> 120 symbols ->
     * ceil(120/16)=8 blocks, all FEC, contiguous block_ids. */
    static uint8_t nals[40][600];
    for (int j = 0; j < 40; j++) {
        nals[j][0] = 0x61;                                      /* anchor */
        for (int i = 1; i < 600; i++) nals[j][i] = (uint8_t)(i*3 + j*7);
        fec_tx_nal(&tx, nals[j], 600);
    }
    fec_tx_flush(&tx);
    int tx_blocks = (int)tx.fec_blocks;

    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);
    uint64_t acc = 0; int dropped_total = 0;
    for (int i = 0; i < g_wn; i++) {
        acc += 5; int drop = 0;                                 /* 5% accumulator */
        if (acc >= 100) { acc -= 100; drop = 1; dropped_total++; }
        if (!drop) fec_rx_feed(&rx, g_wire[i], g_wlen[i], (uint64_t)(i+1)*1000);
    }
    fec_rx_tick(&rx, 0xFFFFFFFFFFFFFFFFull);

    EXPECT(dropped_total > 0, "test actually dropped packets");
    EXPECT(g_nn == 40, "all 40 NALs reassembled despite 5% periodic loss");
    EXPECT((int)rx.fec_blocks <= tx_blocks + 1, "RX block count not inflated (no fabrication)");
    EXPECT(rx.fec_unrecoverable == 0, "no block unrecoverable at 5% (m=2 covers 1/block)");
    int ok = 1;
    for (int j = 0; j < g_nn && j < 40; j++)
        if (g_nlen[j] != 600 || g_nal[j][0] != 0x61) ok = 0;
    EXPECT(ok, "recovered NALs are byte-correct");
}

/* Dropping a whole PACKED packet removes up to 2 symbols. With m>=2 a single
 * dropped count=2 packet (2 symbols) is still recoverable iff <= m losses. */
static void test_packed_packet_loss_recovers(void){
    uint8_t nal[1200]; nal[0]=0x61; for(int i=1;i<1200;i++) nal[i]=(uint8_t)(i*5);
    /* FEC_INNER_PAYLOAD_MAX = FEC_SYMBOL_LEN-3 = 246. 1200B -> ceil(1200/246)=5
     * data syms; m=2 -> n=7 -> 4 wire packets: pairs (0,1),(2,3),(4,5) + tail
     * (6). Drop wire packet 0 (the first pair = syms 0,1) -> 2 losses; present=5
     * >= k=5 so RS recovers (needs m>=2). */
    int drop[1]={0};                 /* wire packet 0 = first pair (syms 0,1) */
    run(2, nal, sizeof nal, drop, 1);
    EXPECT(g_nn==1, "anchor recovered despite a whole packed-pair loss");
    EXPECT(g_nlen[0]==sizeof nal && memcmp(g_nal[0],nal,sizeof nal)==0,
           "byte-identical after packed-pair loss");
}

/* A NAL larger than the old 253 cap reassembles end-to-end (record growth). */
static void test_large_nal_roundtrip(void){
    static uint8_t nal[5000]; nal[0]=0x61; for(int i=1;i<5000;i++) nal[i]=(uint8_t)(i*9+1);
    run(2, nal, sizeof nal, NULL, 0);   /* no drops */
    EXPECT(g_nn==1, "large NAL reassembled");
    EXPECT(g_nlen[0]==sizeof nal && memcmp(g_nal[0],nal,sizeof nal)==0,
           "large NAL byte-identical");
}

/* --- Local helpers for the time-driven-eviction test (mirroring test_fec_rx.c;
 * the e2e harness above only captures NALs, not raw records). ----------------- */
#define ECAP 64
static uint8_t e_rec[ECAP][512]; static size_t e_rlen[ECAP]; static int e_rn;
static void e_rec_emit(const uint8_t *p,size_t n,void *u){ (void)u; if(e_rn<ECAP){memcpy(e_rec[e_rn],p,n);e_rlen[e_rn]=n;e_rn++;} }
static void e_reset(void){ e_rn=0; }
static size_t e_mk_inner(uint8_t *sym, uint8_t flags, const uint8_t *pl, size_t pl_len){
    memset(sym, 0, FEC_SYMBOL_LEN);
    return (size_t)protocol_build(sym, FEC_SYMBOL_LEN, PROTO_MODE_FIXED, flags, pl, pl_len);
}
/* Truncating per-symbol wire length: data (idx<k) ships 3+inner-len, parity full. */
static size_t e_sym_wire_len(const uint8_t *sym, uint8_t idx, uint8_t k){
    if (idx < k) return (size_t)3 + (size_t)unpack_le16(&sym[1]);
    return FEC_SYMBOL_LEN;
}
static size_t e_mk_fec(uint8_t *out, uint8_t bid, uint8_t idx, uint8_t k, uint8_t n,
                       const uint8_t *coded){
    uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
    proto_fec_pair_hdr_pack(payload, bid, idx, k, n, 1);
    size_t l = e_sym_wire_len(coded, idx, k);
    memcpy(payload+FEC_HDR_BYTES, coded, l);
    int len = protocol_build(out, 512, PROTO_MODE_FIXED, PROTO_FLAG_FEC, payload, FEC_HDR_BYTES+l);
    return (size_t)len;
}

/* Large-k + low/variable rate + latency/reorder across the 128-block block_id
 * wrap: a stale open block must be evicted by its TIMEOUT (wall-clock), not by
 * an arrival count, before its block_id is re-used -- so no fabricated
 * cross-block aliasing. k~200 spans many packets per block; the incomplete
 * block is evicted on a tick that crosses FEC_RX_TIMEOUT_NS even though few
 * newer packets have arrived. */
static void test_reasm_eviction_across_wrap(void){
    fec_gf_init();
    FecRx f; fec_rx_init(&f, e_rec_emit, NULL);
    e_reset();
    enum { K = 200, M = 3, N = K + M };
    uint64_t now = 1000;
    int prior_have_leak = 0;
    for (int b = 0; b < 130; b++) {
        uint8_t bid = (uint8_t)(b & 0x7F);          /* what TX would emit */
        int leave_incomplete = (b == 50);           /* a stale open block */
        static uint8_t data[K][FEC_SYMBOL_LEN]; static uint8_t par[M][FEC_SYMBOL_LEN];
        const uint8_t *dp[K]; uint8_t *pp[M];
        for (int i = 0; i < K; i++) {
            uint8_t pl[80]; for (int j = 0; j < 80; j++) pl[j] = (uint8_t)(b*7 + i + j);
            uint8_t fl = PROTO_FLAG_SEQ(i & 0x0F);
            if (i == 0) fl |= PROTO_FLAG_START;
            if (i == K-1) fl |= PROTO_FLAG_END;
            e_mk_inner(data[i], fl, pl, 80); dp[i] = data[i];
        }
        for (int i = 0; i < M; i++) pp[i] = par[i];
        fec_encode(dp, K, M, pp);
        uint8_t rec[512];
        for (int phase = 1; phase >= 0; phase--) {
            for (int i = 0; i < N; i++) {
                if ((i & 1) != phase) continue;
                if (leave_incomplete && i == N - 1) continue;   /* drop -> stays open */
                const uint8_t *s = (i < K) ? data[i] : par[i - K];
                now += (uint64_t)(40 + ((b + i) % 90));         /* low/variable rate */
                fec_rx_feed(&f, rec, e_mk_fec(rec, bid, (uint8_t)i, K, N, s), now);
            }
        }
        now += FEC_RX_TIMEOUT_NS + 1;
        fec_rx_tick(&f, now);
        for (int si = 0; si < FEC_RX_SLOTS; si++)
            if (f.slot[si].used) prior_have_leak = 1; /* a block stayed open across wrap */
    }
    EXPECT(prior_have_leak == 0, "stale block evicted by timeout before block_id re-use");
    /* bid_noninc is a misframe gauge: under clean (non-misframed) feed it only
     * ticks on the legitimate 7-bit wrap (bid 127->0, a >8 step), so it stays
     * tiny -- NOT inflated as it would be under fabricated/aliased blocks. */
    EXPECT(f.bid_noninc <= 2, "bid_noninc gauge bounded (no fabrication)");
}

/* Mixed-class stream: SPS,PPS,IDR (anchors m=5) + ref-P (m=0, plain). With the
 * IDR's first FEC pair dropped, the anchor block still recovers; the ref-P,
 * sent plain, is delivered as long as its own packets arrive. */
static void test_mixed_class_anchor_recovers_ref_plain(void){
    g_wn = 0; g_nn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, 5, 0, 16, &seq, PROTO_MODE_FIXED, tx_capture, NULL);

    static uint8_t sps[12]; sps[0]=0x67; for(int i=1;i<12;i++) sps[i]=(uint8_t)i;
    static uint8_t pps[8];  pps[0]=0x68; for(int i=1;i<8;i++)  pps[i]=(uint8_t)i;
    static uint8_t idr[800];idr[0]=0x65; for(int i=1;i<800;i++)idr[i]=(uint8_t)(i*3);
    static uint8_t ref[300];ref[0]=0x61; for(int i=1;i<300;i++)ref[i]=(uint8_t)(i*7);
    fec_tx_nal(&tx, sps, sizeof sps);
    fec_tx_nal(&tx, pps, sizeof pps);
    fec_tx_nal(&tx, idr, sizeof idr);
    fec_tx_nal(&tx, ref, sizeof ref);
    fec_tx_flush(&tx);

    /* Confirm at least one plain (non-FEC) packet exists = the ref-P went plain.
     * The outer header's flags is byte 0 of each wire packet. */
    int plain=0, fec=0;
    for (int i=0;i<g_wn;i++){ if (g_wire[i][0] & PROTO_FLAG_FEC) fec++; else plain++; }
    EXPECT(plain >= 1, "ref-P at m=0 produced >=1 plain wire packet");
    EXPECT(fec   >= 1, "anchors produced FEC wire packets");

    /* Drop the FIRST FEC packet (an IDR-block pair = up to 2 symbols); m=5 covers it. */
    int first_fec = -1;
    for (int i=0;i<g_wn;i++) if (g_wire[i][0] & PROTO_FLAG_FEC){ first_fec=i; break; }

    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);
    for (int i = 0; i < g_wn; i++)
        if (i != first_fec) fec_rx_feed(&rx, g_wire[i], g_wlen[i], (uint64_t)(i+1)*1000);
    fec_rx_tick(&rx, 0xFFFFFFFFFFFFFFFFull);

    /* All four NALs delivered byte-correct: SPS,PPS,IDR recovered; ref-P plain. */
    EXPECT(g_nn == 4, "all 4 mixed-class NALs delivered (anchor recovered, ref plain)");
    EXPECT(rx.fec_unrecoverable == 0, "no anchor block unrecoverable (m=5 covers 1 pair)");
    int ok = (g_nn==4)
          && g_nlen[0]==sizeof sps && memcmp(g_nal[0],sps,sizeof sps)==0
          && g_nlen[1]==sizeof pps && memcmp(g_nal[1],pps,sizeof pps)==0
          && g_nlen[2]==sizeof idr && memcmp(g_nal[2],idr,sizeof idr)==0
          && g_nlen[3]==sizeof ref && memcmp(g_nal[3],ref,sizeof ref)==0;
    EXPECT(ok, "every delivered NAL is byte-identical");
}

/* m_ref=0: dropping a ref-P plain packet drops only that ref-P; the protected
 * anchor that precedes it is unaffected (no cross-class damage). */
static void test_ref_plain_loss_isolated(void){
    g_wn = 0; g_nn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, 5, 0, 16, &seq, PROTO_MODE_FIXED, tx_capture, NULL);
    static uint8_t idr[300]; idr[0]=0x65; for(int i=1;i<300;i++) idr[i]=(uint8_t)i;
    static uint8_t ref[300]; ref[0]=0x61; for(int i=1;i<300;i++) ref[i]=(uint8_t)(i*9);
    fec_tx_nal(&tx, idr, sizeof idr);
    fec_tx_nal(&tx, ref, sizeof ref);
    fec_tx_flush(&tx);

    /* Drop the first plain packet (the ref-P's START chunk); flags is byte 0. */
    int first_plain = -1;
    for (int i=0;i<g_wn;i++) if (!(g_wire[i][0] & PROTO_FLAG_FEC)){ first_plain=i; break; }
    EXPECT(first_plain >= 0, "a plain ref-P packet exists to drop");

    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);
    for (int i = 0; i < g_wn; i++)
        if (i != first_plain) fec_rx_feed(&rx, g_wire[i], g_wlen[i], (uint64_t)(i+1)*1000);
    fec_rx_tick(&rx, 0xFFFFFFFFFFFFFFFFull);

    /* The IDR is delivered intact; only the ref-P is lost (1 NAL, not 0). */
    EXPECT(g_nn == 1, "anchor delivered; only the unprotected ref-P dropped");
    EXPECT(g_nlen[0]==sizeof idr && memcmp(g_nal[0],idr,sizeof idr)==0,
           "IDR byte-identical despite an adjacent ref-P plain loss");
}

/* Drop a burst of `blen` consecutive wire packets starting at index `bstart`;
 * replay the rest through fec_rx -> reasm. Reuses the file's g_wire capture and
 * nal_capture sink. Returns the reconstructed NAL length (0 if not delivered). */
static size_t replay_with_burst(int bstart, int blen){
    g_nn = 0;
    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);
    for (int i = 0; i < g_wn; i++) {
        if (i >= bstart && i < bstart + blen) continue;     /* burst erasure */
        fec_rx_feed(&rx, g_wire[i], g_wlen[i], 1000);
    }
    fec_rx_tick(&rx, 1000ull + FEC_RX_TIMEOUT_NS);
    reasm_tick(&r);
    return g_nn >= 1 ? g_nlen[0] : 0;
}

/* THE WHOLE POINT of pairwise column interleave: a CONCENTRATED BURST of
 * consecutive on-wire packet losses that legacy (2-symbols-of-ONE-block-per-
 * packet) packing could NOT survive. With k_bound=16, a 9000 B IDR is 37 data
 * symbols -> block0(k=16),block1(k=16) both full + interleaved as 25 column
 * packets [wire 0..24] (each packet j = (block0[j],block1[j])), plus a legacy
 * tail block (k=5) at [wire 25..]. A burst of b consecutive ILV packets hits b
 * DIFFERENT columns -> each of the two blocks loses exactly b symbols. With m=9
 * a block tolerates up to 9 column losses: a 4- AND a 5-packet burst recover.
 * Legacy packing would put 2 symbols of one block per packet -> a 5-packet burst
 * costs that block ~10 symbols > m=9 -> UNRECOVERABLE. Interleave is the
 * discriminator. The burst window (bstart=2,blen=5) lands squarely inside the
 * single interleaved pair [0..24], not the legacy tail. */
static void test_interleave_survives_burst(void){
    static uint8_t idr[9000]; idr[0]=0x65; for(size_t i=1;i<sizeof idr;i++) idr[i]=(uint8_t)(i*11+3);
    g_wn=0;
    uint8_t seq=0; FecTx tx; fec_tx_init(&tx, 9, 2, 16, &seq, PROTO_MODE_FIXED, tx_capture, NULL);
    fec_tx_nal(&tx, idr, sizeof idr);
    fec_tx_flush(&tx);

    /* Verify the layout assumption: >=25 column-interleaved (FEC_ILV) packets
     * exist, so the [2,7) burst lands inside one interleaved pair, not the tail. */
    int ilv=0;
    for (int i=0;i<g_wn;i++) if ((g_wire[i][0] & PROTO_FLAG_FEC) && (g_wire[i][0] & PROTO_FLAG_FEC_ILV)) ilv++;
    EXPECT(ilv >= 25, "two full k=16 blocks emitted column-interleaved (>=25 ILV packets)");

    /* A 4-packet burst: each block loses 4 <= m=9 -> recover byte-identical. */
    size_t got = replay_with_burst(2, 4);
    EXPECT(got==sizeof idr, "interleaved IDR survives a 4-packet burst");
    EXPECT(g_nn>=1 && memcmp(g_nal[0], idr, sizeof idr)==0, "recovered IDR is byte-identical (4-burst)");

    /* A 5-packet burst: interleave spreads to 5<=9 per block; legacy m/2=4 would NOT. */
    size_t got5 = replay_with_burst(2, 5);
    EXPECT(got5==sizeof idr, "interleave survives 5-pkt burst (legacy m/2=4 would not)");
    EXPECT(g_nn>=1 && memcmp(g_nal[0], idr, sizeof idr)==0, "recovered IDR is byte-identical (5-burst)");
}

/* Truncation: a small ref-P FEC block must (a) recover byte-identical and
 * (b) put a FEC packet on the wire SHORTER than the full-width 511 B ceiling.
 * Pre-change TX pads to 511 -> assertion (b) fails; post-change it truncates. */
static void test_small_block_wire_shrinks(void){
    uint8_t nal[40]; nal[0]=0x61;                 /* REF -> m_ref block */
    for(int i=1;i<40;i++) nal[i]=(uint8_t)(i*5);
    run(2, nal, sizeof nal, NULL, 0);             /* m_anchor=m_ref=2, no loss */
    EXPECT(g_nn==1 && g_nlen[0]==sizeof nal && memcmp(g_nal[0],nal,sizeof nal)==0,
           "small ref-P recovers byte-identical through truncation");
    int saw_short_fec = 0;
    for (int i = 0; i < g_wn; i++) {
        /* Gate on a count=2 PAIR (pair-header byte 0 high bit, at wire offset 3 =
         * after the 3-byte FIXED proto header): a count=1 tail is naturally < 511
         * pre-change and would false-trigger; only a truncated data symbol shrinks
         * a count=2 pair below the full-width ceiling. */
        if ((g_wire[i][0] & PROTO_FLAG_FEC) && (g_wire[i][3] & 0x80) &&
            g_wlen[i] < (size_t)(3 + FEC_HDR_BYTES + 2*FEC_SYMBOL_LEN))   /* < 511 */
            saw_short_fec = 1;
    }
    EXPECT(saw_short_fec, "a FEC packet is shorter than the full-width 511 B frame");
}

/* SPS+PPS share GRP_PARAMSET -> one k=2 block with TWO mid-block partial data
 * symbols. Both must round-trip byte-identical through truncation. (Verified:
 * the two NALs pack into ONE k=2,n=11 anchor block; wire pkt 0 = pair(0,1) =
 * both partial data symbols, len 33 << 511.) No packets dropped -> this tests
 * the TRUNCATION round-trip, not RS recovery. */
static void test_paramset_two_partials_roundtrip(void){
    g_wn = 0; g_nn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, 9, 2, 16, &seq, PROTO_MODE_FIXED, tx_capture, NULL);
    uint8_t sps[12]; sps[0]=0x67; for(int i=1;i<12;i++) sps[i]=(uint8_t)(i*3);
    uint8_t pps[8];  pps[0]=0x68; for(int i=1;i<8;i++)  pps[i]=(uint8_t)(i*5);
    fec_tx_nal(&tx, sps, sizeof sps);
    fec_tx_nal(&tx, pps, sizeof pps);
    fec_tx_flush(&tx);
    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);
    for (int i = 0; i < g_wn; i++) fec_rx_feed(&rx, g_wire[i], g_wlen[i], (uint64_t)(i+1)*1000);
    fec_rx_tick(&rx, 0xFFFFFFFFFFFFFFFFull);
    EXPECT(g_nn==2, "SPS+PPS both reassembled");
    EXPECT(g_nlen[0]==sizeof sps && memcmp(g_nal[0],sps,sizeof sps)==0, "SPS byte-identical");
    EXPECT(g_nlen[1]==sizeof pps && memcmp(g_nal[1],pps,sizeof pps)==0, "PPS byte-identical");
}

/* A truncated data symbol dropped on the wire must be RS-recovered from the
 * FULL-WIDTH parity -- proves the RX zero-extends the survivors to 252 before
 * decode (the parity was computed over the zero-padded 252 B symbols). */
static void test_truncated_data_recovered_from_parity(void){
    /* 300B ref-P, m=2 -> 2 data syms (idx0 full @249payload, idx1 PARTIAL @53B)
     * + 2 parity (idx2,3, full width); k=2,n=4. Legacy emit: pair(0,1) [both
     * data], pair(2,3) [both parity]. Drop wire pkt 0 (data syms 0,1 incl. the
     * truncated idx1) -> present = 2 full-width parity = k -> RS recovers both. */
    uint8_t nal[300]; nal[0]=0x61; for(int i=1;i<300;i++) nal[i]=(uint8_t)(i*5);
    int drop[1]={0};
    run(2, nal, sizeof nal, drop, 1);
    EXPECT(g_nn==1, "ref-P recovered despite losing both truncated data symbols");
    EXPECT(g_nlen[0]==sizeof nal && memcmp(g_nal[0],nal,sizeof nal)==0,
           "byte-identical recovery from full-width parity (zero-extend correct)");
}

/* An interleaved multi-block IDR with truncated data columns survives a burst
 * that knocks out two whole column packets; ILV spreads the loss across blocks
 * so neither block loses more than m. */
static void test_ilv_truncated_survives_burst(void){
    g_wn = 0; g_nn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, 2, 2, 16, &seq, PROTO_MODE_FIXED, tx_capture, NULL);
    /* Two 16*249 IDR anchors (same GRP_IDR) feed 17 data syms each. block0 (a's
     * first 16 syms) and block1 (a's 2B tail + b's first 15 syms) are both
     * k=16,n=18 -> they interleave pairwise into 18 column packets [wire 0..17]
     * (packet j = block0[j],block1[j]); a small k=2 tail block [wire 18,19]
     * carries b's remainder. */
    static uint8_t a[16*249], b[16*249];
    a[0]=0x65; for(size_t i=1;i<sizeof a;i++) a[i]=(uint8_t)(i*3);
    b[0]=0x65; for(size_t i=1;i<sizeof b;i++) b[i]=(uint8_t)(i*7+1);
    fec_tx_nal(&tx, a, sizeof a);
    fec_tx_nal(&tx, b, sizeof b);
    fec_tx_flush(&tx);
    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);
    /* Drop the first two ILV column packets (wire idx 0,1): each carries column
     * 0 / column 1 of BOTH blocks, so each block loses exactly 2 = m -> both
     * recover. The tail block (wire 18,19) is fed intact. */
    for (int i = 0; i < g_wn; i++)
        if (i >= 2) fec_rx_feed(&rx, g_wire[i], g_wlen[i], (uint64_t)(i+1)*1000);
    fec_rx_tick(&rx, 0xFFFFFFFFFFFFFFFFull);
    EXPECT(g_nn==2, "both interleaved IDR NALs recovered after a 2-column burst");
    EXPECT(g_nlen[0]==sizeof a && memcmp(g_nal[0],a,sizeof a)==0, "block A byte-identical");
    EXPECT(g_nlen[1]==sizeof b && memcmp(g_nal[1],b,sizeof b)==0, "block B byte-identical");
}

/* --- Block (timing) interleaver discriminator ----------------------------- *
 * Four separate small same-group (IDR) NALs, each sized to exactly k_bound=4
 * data symbols so each soft-closes into its OWN k=4,m=2,n=6 block. Default
 * (depth OFF) emission column-pairs the four equal blocks into TWO interleaved
 * pairs [wire 0..5]=(blk0,blk1), [wire 6..11]=(blk2,blk3); a 4-packet burst at
 * wire 0 lands on columns 0..3 of blk0 AND blk1 -> each loses 4 > m=2 -> BOTH
 * unrecoverable (2 NALs lost). The block interleaver at depth 4 round-robins the
 * four blocks' packets by position, so the SAME 4-packet burst spreads 1 packet
 * to each of the four blocks -> each loses <= 1 <= m -> all four NALs recover
 * byte-identical. The column scheme cannot fix this (it only spreads across the
 * 2 blocks of one pair); depth-4 spreads across all 4 -> the discriminator. */
#define BI_NN 4
#define BI_SYMS 4                    /* == k_bound -> each NAL is its own block   */
#define BI_NLEN (BI_SYMS * 246)      /* 4 data symbols (FEC_INNER_PAYLOAD_MAX=249) */
static uint8_t bi_nal[BI_NN][BI_NLEN];

static void bi_build(int depth){
    g_wn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, 2, 2, BI_SYMS, &seq, PROTO_MODE_FIXED, tx_capture, NULL);
    if (depth >= 2) fec_tx_set_block_depth(&tx, depth);
    for (int j = 0; j < BI_NN; j++) fec_tx_nal(&tx, bi_nal[j], BI_NLEN);
    fec_tx_flush(&tx);
}
/* Replay the captured wire with a burst erasure [bstart, bstart+blen); return the
 * count of the BI_NN NALs that reassembled byte-identical (order-independent). */
static int bi_survivors(int bstart, int blen){
    g_nn = 0;
    Reasm r; reasm_init(&r, nal_capture, NULL);
    FecRx rx; fec_rx_init(&rx, fec_to_reasm, &r);
    for (int i = 0; i < g_wn; i++) {
        if (i >= bstart && i < bstart + blen) continue;
        fec_rx_feed(&rx, g_wire[i], g_wlen[i], 1000);
    }
    fec_rx_tick(&rx, 1000ull + FEC_RX_TIMEOUT_NS);
    reasm_tick(&r);
    int ok = 0;
    for (int j = 0; j < BI_NN; j++)
        for (int g = 0; g < g_nn; g++)
            if (g_nlen[g] == BI_NLEN && memcmp(g_nal[g], bi_nal[j], BI_NLEN) == 0) { ok++; break; }
    return ok;
}

static void test_block_interleave_survives_cross_nal_burst(void){
    for (int j = 0; j < BI_NN; j++) {
        bi_nal[j][0] = 0x65;                                  /* IDR (anchor, m=2) */
        for (size_t i = 1; i < BI_NLEN; i++) bi_nal[j][i] = (uint8_t)(i*3 + j*101 + 7);
    }
    enum { L = 4 };   /* tuned: legacy/column dies, depth-4 lives unambiguously */

    /* Control (depth OFF): the 4-packet burst overwhelms one column pair's two
     * blocks -> at least one NAL is NOT recovered. */
    bi_build(0);
    EXPECT(g_wn == 12, "4 small IDR blocks emit 12 wire packets at depth OFF");
    int c0 = bi_survivors(0, L);
    EXPECT(c0 < BI_NN, "depth-OFF: a 4-pkt cross-NAL burst loses >=1 NAL (column can't spread it)");

    /* Block depth 4: the SAME burst spreads 1 packet per block -> all recover. */
    bi_build(4);
    EXPECT(g_wn == 12, "depth-4 emits the SAME 12 wire packets (airtime-neutral, only reordered)");
    int c4 = bi_survivors(0, L);
    EXPECT(c4 == BI_NN, "depth-4: the same burst spreads ceil(L/4)<=m -> ALL 4 NALs byte-identical");
    EXPECT(c4 > c0, "block interleaver strictly beats depth-OFF on the cross-NAL burst");
}

/* Guard the production-untouched invariant: at depth 0 the block interleaver
 * emits the EXACT same wire packet stream (count, lengths, bytes) as today for a
 * representative mixed multi-NAL input. Built once with a normal FecTx and once
 * with depth explicitly set to 0 -- they must be byte-identical packet-for-packet. */
static void test_block_depth_off_is_byte_identical(void){
    static uint8_t sps[12]; sps[0]=0x67; for(int i=1;i<12;i++) sps[i]=(uint8_t)(i*3);
    static uint8_t pps[8];  pps[0]=0x68; for(int i=1;i<8;i++)  pps[i]=(uint8_t)(i*5);
    static uint8_t idr[3000];idr[0]=0x65; for(int i=1;i<3000;i++)idr[i]=(uint8_t)(i*7+1);
    static uint8_t ref[400]; ref[0]=0x61; for(int i=1;i<400;i++) ref[i]=(uint8_t)(i*9);
    static uint8_t disp[200];disp[0]=0x01;for(int i=1;i<200;i++) disp[i]=(uint8_t)(i*11);

    /* Reference: plain FecTx, no depth set (production path). */
    g_wn = 0;
    uint8_t seq = 0;
    FecTx tx; fec_tx_init(&tx, 9, 2, 16, &seq, PROTO_MODE_FIXED, tx_capture, NULL);
    fec_tx_nal(&tx, sps, sizeof sps); fec_tx_nal(&tx, pps, sizeof pps);
    fec_tx_nal(&tx, idr, sizeof idr); fec_tx_nal(&tx, ref, sizeof ref);
    fec_tx_nal(&tx, disp, sizeof disp);
    fec_tx_flush(&tx);
    int ref_wn = g_wn;
    static uint8_t ref_wire[WCAP][512]; static size_t ref_wlen[WCAP];
    for (int i = 0; i < g_wn; i++) { memcpy(ref_wire[i], g_wire[i], g_wlen[i]); ref_wlen[i] = g_wlen[i]; }

    /* Same input through a FecTx with block depth explicitly set to 0. */
    g_wn = 0;
    uint8_t seq2 = 0;
    FecTx tx2; fec_tx_init(&tx2, 9, 2, 16, &seq2, PROTO_MODE_FIXED, tx_capture, NULL);
    fec_tx_set_block_depth(&tx2, 0);
    fec_tx_nal(&tx2, sps, sizeof sps); fec_tx_nal(&tx2, pps, sizeof pps);
    fec_tx_nal(&tx2, idr, sizeof idr); fec_tx_nal(&tx2, ref, sizeof ref);
    fec_tx_nal(&tx2, disp, sizeof disp);
    fec_tx_flush(&tx2);

    EXPECT(g_wn == ref_wn, "depth 0 emits the same wire packet COUNT as production");
    int identical = (g_wn == ref_wn);
    for (int i = 0; i < g_wn && i < ref_wn; i++)
        if (g_wlen[i] != ref_wlen[i] || memcmp(g_wire[i], ref_wire[i], g_wlen[i]) != 0) identical = 0;
    EXPECT(identical, "depth 0 wire stream is byte-identical to production (untouched)");
}

int main(void){
    fec_gf_init();
    test_block_interleave_survives_cross_nal_burst();
    test_block_depth_off_is_byte_identical();
    test_anchor_survives_one_loss();
    test_anchor_drops_beyond_m();
    test_disposable_unprotected();
    test_small_block_wire_shrinks();
    test_multiblock_stream_periodic_loss();
    test_packed_packet_loss_recovers();
    test_large_nal_roundtrip();
    test_reasm_eviction_across_wrap();
    test_mixed_class_anchor_recovers_ref_plain();
    test_ref_plain_loss_isolated();
    test_interleave_survives_burst();
    test_paramset_two_partials_roundtrip();
    test_truncated_data_recovered_from_parity();
    test_ilv_truncated_survives_burst();
    if(g_failures){fprintf(stderr,"test_fec_e2e: %d failure(s)\n",g_failures);return 1;}
    printf("test_fec_e2e: OK\n"); return 0;
}
