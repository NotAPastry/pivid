#include "../apps/pivid-rx/src/fec_rx.h"
#include "protocol.h"
#include "../common/fec.h"
#include <stdio.h>
#include <string.h>

static int g_failures=0;
#define EXPECT(c,m) do{ if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,m);g_failures++;} }while(0)

/* Recorder of records emitted to reasm. */
#define CAP 64
static uint8_t g_rec[CAP][512]; static size_t g_rlen[CAP]; static int g_rn;
static void rec_emit(const uint8_t *p,size_t n,void *u){ (void)u; if(g_rn<CAP){memcpy(g_rec[g_rn],p,n);g_rlen[g_rn]=n;g_rn++;} }
static void reset(void){ g_rn=0; }

/* True when no FEC slot is currently collecting a block (the post-refactor
 * equivalent of the old single-block `open==0 && have==0` invariant). */
static int no_slot_open(const FecRx *f){
    for (int i=0;i<FEC_RX_SLOTS;i++) if (f->slot[i].used) return 0;
    return 1;
}

/* Wire length of one full 252 B symbol buffer at block index `idx`: a data
 * symbol (idx<k) is truncated to 3 + its inner len; parity (idx>=k) is full. */
static size_t sym_wire_len(const uint8_t *sym, uint8_t idx, uint8_t k){
    if (idx < k) return (size_t)3 + (size_t)unpack_le16(&sym[1]);
    return FEC_SYMBOL_LEN;
}
/* Build a FEC-coded outer record carrying one symbol (truncated per the rule). */
static size_t mk_fec(uint8_t *out, uint8_t bid, uint8_t idx, uint8_t k, uint8_t n,
                     const uint8_t *coded) {
    uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
    proto_fec_pair_hdr_pack(payload, bid, idx, k, n, 1);
    size_t l = sym_wire_len(coded, idx, k);
    memcpy(payload+FEC_HDR_BYTES, coded, l);
    int len = protocol_build(out, 512, PROTO_MODE_FIXED, PROTO_FLAG_FEC, payload, FEC_HDR_BYTES+l);
    return (size_t)len;
}
/* Build a count=2 FEC record carrying two consecutive symbols (each truncated). */
static size_t mk_fec_pair(uint8_t *out, uint8_t bid, uint8_t base, uint8_t k, uint8_t n,
                          const uint8_t *s0, const uint8_t *s1) {
    uint8_t payload[FEC_HDR_BYTES + 2*FEC_SYMBOL_LEN];
    proto_fec_pair_hdr_pack(payload, bid, base, k, n, 2);
    size_t l0 = sym_wire_len(s0, base, k);
    size_t l1 = sym_wire_len(s1, (uint8_t)(base+1), k);
    memcpy(payload + FEC_HDR_BYTES, s0, l0);
    memcpy(payload + FEC_HDR_BYTES + l0, s1, l1);
    int len = protocol_build(out, 512, PROTO_MODE_FIXED, PROTO_FLAG_FEC,
                             payload, FEC_HDR_BYTES + l0 + l1);
    return (size_t)len;
}

/* An inner data packet (the RS symbol's real bytes): [flags][len][payload]. */
static size_t mk_inner(uint8_t *sym, uint8_t flags, const uint8_t *pl, size_t pl_len) {
    memset(sym, 0, FEC_SYMBOL_LEN);
    return (size_t)protocol_build(sym, FEC_SYMBOL_LEN, PROTO_MODE_FIXED, flags, pl, pl_len);
}

static void test_passthrough_fec0(void){
    reset();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t rec[64]; uint8_t pl[20]; for(int i=0;i<20;i++) pl[i]=(uint8_t)i;
    int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_START|PROTO_FLAG_END, pl, 20);
    fec_rx_feed(&f, rec, (size_t)len, 1000);
    EXPECT(g_rn==1, "FEC=0 passes straight through");
    EXPECT(g_rec[0][1]==20, "passthrough record intact");
}

static void test_recover_one_loss(void){
    reset();
    fec_gf_init();
    /* k=3 data inner packets, m=2 parity. */
    uint8_t data[3][FEC_SYMBOL_LEN]; uint16_t dlen[3];
    for(int i=0;i<3;i++){
        uint8_t pl[100]; for(int j=0;j<100;j++) pl[j]=(uint8_t)(i*100+j);
        uint8_t flags = PROTO_FLAG_SEQ(i); if(i==0)flags|=PROTO_FLAG_START; if(i==2)flags|=PROTO_FLAG_END;
        dlen[i]=(uint16_t)mk_inner(data[i], flags, pl, 100);
    }
    uint8_t par[2][FEC_SYMBOL_LEN]; const uint8_t *dp[3]; uint8_t *pp[2];
    for(int i=0;i<3;i++) dp[i]=data[i];
    for(int i=0;i<2;i++) pp[i]=par[i];
    fec_encode(dp,3,2,pp);

    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t rec[512];
    /* Feed data idx 0 and 2 (DROP idx 1), plus parity idx 3 and 4. On the wire
     * every symbol is a full FEC_SYMBOL_LEN (data buffers are zero-padded); the
     * real inner record length is recovered downstream from the embedded u16. */
    fec_rx_feed(&f, rec, mk_fec(rec,7,0,3,5,data[0]), 1000);
    fec_rx_feed(&f, rec, mk_fec(rec,7,2,3,5,data[2]), 1000);
    fec_rx_feed(&f, rec, mk_fec(rec,7,3,3,5,par[0]), 1000);
    fec_rx_feed(&f, rec, mk_fec(rec,7,4,3,5,par[1]), 1000);
    /* data idx1 is absent -> slot_data_complete is false -> no early-close;
     * have==4 < n==5 so it doesn't close on count either. Trigger via timeout tick. */
    fec_rx_tick(&f, 1000 + FEC_RX_TIMEOUT_NS + 1);
    /* Block closed: 4 present (idx 0,2,3,4) >= k=3 -> RS recovers idx 1. */
    EXPECT(g_rn==3, "all 3 data records emitted in order");
    EXPECT(f.fec_recovered==1, "block needed + achieved recovery");
    EXPECT(g_rlen[0]==dlen[0] && g_rlen[1]==dlen[1] && g_rlen[2]==dlen[2], "lengths preserved");
    EXPECT(memcmp(g_rec[1], data[1], dlen[1])==0, "recovered idx1 byte-identical");
}

static void test_unrecoverable_emits_survivors(void){
    reset();
    fec_gf_init();
    uint8_t data[3][FEC_SYMBOL_LEN]; uint16_t dlen[3];
    for(int i=0;i<3;i++){ uint8_t pl[40]; for(int j=0;j<40;j++) pl[j]=(uint8_t)(i+j);
        dlen[i]=(uint16_t)mk_inner(data[i], PROTO_FLAG_SEQ(i), pl, 40); }
    uint8_t par[1][FEC_SYMBOL_LEN]; const uint8_t *dp[3]; uint8_t *pp[1];
    for(int i=0;i<3;i++) dp[i]=data[i];
    pp[0]=par[0];
    fec_encode(dp,3,1,pp);                       /* m=1: can fix 1 loss only */
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t rec[512];
    /* Drop idx 1 AND idx 2 (2 losses > m=1). Feed idx0 + parity, then close via FEC=0. */
    fec_rx_feed(&f, rec, mk_fec(rec,9,0,3,4,data[0]), 1000);
    fec_rx_feed(&f, rec, mk_fec(rec,9,3,3,4,par[0]), 1000);
    uint8_t pl[5]={1,2,3,4,5}; int dl=protocol_build(rec,sizeof rec,PROTO_MODE_FIXED,PROTO_FLAG_START|PROTO_FLAG_END,pl,5);
    fec_rx_feed(&f, rec, (size_t)dl, 1000);      /* FEC=0 closes the block */
    EXPECT(f.fec_unrecoverable==1, "block flagged unrecoverable");
    EXPECT(g_rn==2, "1 survivor + the closing FEC=0 record");
    EXPECT(g_rlen[0]==dlen[0], "survivor emitted with correct length");
}

static void test_packed_pair_recovers_loss(void){
    reset();
    fec_gf_init();
    /* k=3 data, m=2 parity, n=5. */
    uint8_t data[3][FEC_SYMBOL_LEN]; uint16_t dlen[3];
    for(int i=0;i<3;i++){ uint8_t pl[100]; for(int j=0;j<100;j++) pl[j]=(uint8_t)(i*100+j);
        uint8_t flags=PROTO_FLAG_SEQ(i); if(i==0)flags|=PROTO_FLAG_START; if(i==2)flags|=PROTO_FLAG_END;
        dlen[i]=(uint16_t)mk_inner(data[i],flags,pl,100); }
    uint8_t par[2][FEC_SYMBOL_LEN]; const uint8_t *dp[3]; uint8_t *pp[2];
    for(int i=0;i<3;i++) dp[i]=data[i];
    for(int i=0;i<2;i++) pp[i]=par[i];
    fec_encode(dp,3,2,pp);

    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t rec[512];
    /* Pair (0,1): DROP it (lose data idx 0 and 1). Pair (2,3): deliver.
     * Tail (4): deliver. -> present {2,3,4} = 3 >= k=3 -> RS recovers 0,1. */
    fec_rx_feed(&f, rec, mk_fec_pair(rec,7,2,3,5,data[2],par[0]), 1000);   /* idx 2,3 */
    fec_rx_feed(&f, rec, mk_fec(rec,7,4,3,5,par[1]), 1000); /* idx 4 (count=1) */
    fec_rx_tick(&f, 1000 + FEC_RX_TIMEOUT_NS + 1);
    EXPECT(g_rn==3, "all 3 data records emitted");
    EXPECT(f.fec_recovered==1, "RS recovered the dropped pair");
    EXPECT(g_rlen[0]==dlen[0] && memcmp(g_rec[0],data[0],dlen[0])==0, "idx0 recovered");
    EXPECT(g_rlen[1]==dlen[1] && memcmp(g_rec[1],data[1],dlen[1])==0, "idx1 recovered");
}

/* RX validation: a packet that fails either pre-demux check (pair-header sanity
 * or STRICT length equality) is dropped (counted), with NO symbol fed to the
 * decoder and NO OOB. */
static void test_rx_validation_drops(void){
    fec_gf_init();
    uint8_t sym[FEC_SYMBOL_LEN]; for(int j=0;j<FEC_SYMBOL_LEN;j++) sym[j]=(uint8_t)(j+1);
    uint8_t sym2[FEC_SYMBOL_LEN]; for(int j=0;j<FEC_SYMBOL_LEN;j++) sym2[j]=(uint8_t)(j*3+2);

    /* (a) Sanity-violating header: k > n. */
    {
        reset();
        FecRx f; fec_rx_init(&f, rec_emit, NULL);
        uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
        proto_fec_pair_hdr_pack(payload, 7, 0, 9, 5, 1); /* k=9 > n=5 */
        memcpy(payload + FEC_HDR_BYTES, sym, FEC_SYMBOL_LEN);
        uint8_t rec[512];
        int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_FEC,
                                 payload, FEC_HDR_BYTES + FEC_SYMBOL_LEN);
        fec_rx_feed(&f, rec, (size_t)len, 1000);
        EXPECT(f.drops_sanity==1, "k>n header dropped");
        EXPECT(no_slot_open(&f), "no symbol fed on sanity violation");
    }

    /* (b) count=2 header backed by only ONE symbol's worth of bytes (a misframe).
     * The length walk rejects it (drops_len) before any store: the first data
     * symbol's bytes parse to a self-described inner len > FEC_SYMBOL_LEN, so the
     * per-symbol bound fails -- the truncated-format replacement for the old
     * strict `==` (which a `>=` would have OOB-read past). */
    {
        reset();
        FecRx f; fec_rx_init(&f, rec_emit, NULL);
        uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
        proto_fec_pair_hdr_pack(payload, 7, 0, 3, 5, 2); /* count=2 */
        memcpy(payload + FEC_HDR_BYTES, sym2, FEC_SYMBOL_LEN);             /* only 1 sym */
        uint8_t rec[512];
        int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_FEC,
                                 payload, FEC_HDR_BYTES + FEC_SYMBOL_LEN); /* truncated */
        fec_rx_feed(&f, rec, (size_t)len, 1000);
        EXPECT(f.drops_len==1, "truncated count=2 packet dropped (parsed_len too small)");
        EXPECT(no_slot_open(&f), "no symbol fed on truncation");
    }

    /* (c) count=1 header followed by trailing garbage: payload too LARGE for the
     * single symbol. The length walk rejects it (drops_len): the data symbol's
     * bytes parse to an inner len > FEC_SYMBOL_LEN (or, were it in range, the
     * trailing bytes would break the exact total==plen) -- the truncated-format
     * replacement for the old strict `==` that a `>=` would have accepted. */
    {
        reset();
        FecRx f; fec_rx_init(&f, rec_emit, NULL);
        uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN + 7];
        proto_fec_pair_hdr_pack(payload, 7, 0, 3, 5, 1); /* count=1 */
        memcpy(payload + FEC_HDR_BYTES, sym, FEC_SYMBOL_LEN);
        for (int j = 0; j < 7; j++)                       /* trailing garbage */
            payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN + j] = (uint8_t)(0xA0 + j);
        uint8_t rec[512];
        int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_FEC,
                                 payload, sizeof payload);  /* over-long for count=1 */
        fec_rx_feed(&f, rec, (size_t)len, 1000);
        EXPECT(f.drops_len==1, "over-long count=1 packet dropped (parsed_len too large)");
        EXPECT(no_slot_open(&f), "no symbol fed on trailing-garbage misframe");
    }
}

/* A full count=2 pair now exactly fills the 511 B FLRC frame (3 + 4 + 2*252),
 * with no trailing pad. Drains into a >=511 B buffer with no OOB access. */
static void test_fixed_frame_511_no_oob(void){
    reset();
    fec_gf_init();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    /* Two genuinely-full data symbols (inner payload 249 -> inner len 252 ->
     * self-describe as 252), so the count=2 pair still fills 511 with no pad. */
    uint8_t sym[FEC_SYMBOL_LEN], sym1[FEC_SYMBOL_LEN];
    uint8_t pl[249]; for(int j=0;j<249;j++) pl[j]=(uint8_t)(j*2+1);
    mk_inner(sym,  PROTO_FLAG_START|PROTO_FLAG_SEQ(0), pl, 249);
    for(int j=0;j<249;j++) pl[j]=(uint8_t)(j+9);
    mk_inner(sym1, PROTO_FLAG_END|PROTO_FLAG_SEQ(1),   pl, 249);
    struct { uint8_t buf[511]; uint8_t guard; } b;
    memset(&b, 0xCC, sizeof b);
    size_t n = mk_fec_pair(b.buf, 7, 0, 3, 5, sym, sym1);   /* idx 0,1 < k=3 -> full data */
    EXPECT(n == (size_t)(3 + FEC_HDR_BYTES + 2*FEC_SYMBOL_LEN), "full count=2 frame == 511");
    EXPECT(n == 511, "two full data symbols fill the FLRC frame exactly");
    fec_rx_feed(&f, b.buf, 511, 1000);
    EXPECT(b.guard == 0xCC, "no OOB past the 511 B fixed-mode frame buffer");
    EXPECT(f.drops_len==0 && f.drops_sanity==0, "valid 511 B fixed-mode frame accepted");
}

/* A data symbol whose embedded inner length is > 249 (3 + len > FEC_SYMBOL_LEN)
 * is corruption/misframe a legit TX never produces. With truncated symbols the
 * length WALK now catches it (drops_len) before any store; emit_data_symbol's
 * drops_inner_len guard only covers RS-reconstructed symbols. */
static void test_inner_len_overrange_rejected(void) {
    reset();
    fec_gf_init();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    /* k=1 n=1 count=1 data symbol claiming inner len 250 -> sl = 253 > 252. The
     * length walk rejects it (drops_len) before any store; emit_data_symbol's
     * drops_inner_len guard now covers only RS-reconstructed symbols. */
    uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
    proto_fec_pair_hdr_pack(payload, 0, 0, 1, 1, 1);
    payload[FEC_HDR_BYTES + 0] = PROTO_FLAG_START | PROTO_FLAG_END;
    pack_le16(&payload[FEC_HDR_BYTES + 1], 250);          /* 3 + 250 = 253 > 252 */
    uint8_t rec[512];
    int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_FEC,
                             payload, FEC_HDR_BYTES + FEC_SYMBOL_LEN);
    fec_rx_feed(&f, rec, (size_t)len, 1000);
    EXPECT(g_rn == 0, "over-range inner len NOT emitted");
    EXPECT(f.drops_len == 1, "walk rejects sl>252 (drops_len, not drops_inner_len)");
}

/* Two back-to-back legacy blocks (ids 5,6), each k=2 m=0 all-data, must both
 * emit their data symbols in order across the block boundary. */
/* Build a count=2 INTERLEAVED column packet: id field = block_A, base = column j,
 * symbols = (A[j], B[j]). */
static size_t mk_fec_ilv(uint8_t *out, uint8_t blockA, uint8_t col, uint8_t k, uint8_t n,
                         const uint8_t *aj, const uint8_t *bj) {
    uint8_t payload[FEC_HDR_BYTES + 2*FEC_SYMBOL_LEN];
    proto_fec_pair_hdr_pack(payload, blockA, col, k, n, 2);
    size_t la = sym_wire_len(aj, col, k);
    size_t lb = sym_wire_len(bj, col, k);
    memcpy(payload + FEC_HDR_BYTES, aj, la);
    memcpy(payload + FEC_HDR_BYTES + la, bj, lb);
    return (size_t)protocol_build(out, 512, PROTO_MODE_FIXED,
                                  PROTO_FLAG_FEC|PROTO_FLAG_FEC_ILV,
                                  payload, FEC_HDR_BYTES + la + lb);
}

/* Two interleaved all-data blocks (A=id6 k=2, B=id7 k=2), columns 0..1.
 * Emission must be A's two symbols (SEQ 0,1) then B's (SEQ 2,3). */
static void test_ilv_pair_assembles(void){
    reset();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t A0[FEC_SYMBOL_LEN],A1[FEC_SYMBOL_LEN],B0[FEC_SYMBOL_LEN],B1[FEC_SYMBOL_LEN];
    uint8_t p[4];
    p[0]=0;p[1]=1;p[2]=2;p[3]=3;  mk_inner(A0, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), p, 4);
    p[0]=4; mk_inner(A1, PROTO_FLAG_END|PROTO_FLAG_SEQ(1), p, 4);
    p[0]=5; mk_inner(B0, PROTO_FLAG_START|PROTO_FLAG_SEQ(2), p, 4);
    p[0]=6; mk_inner(B1, PROTO_FLAG_END|PROTO_FLAG_SEQ(3), p, 4);
    uint8_t rec[600]; size_t L;
    L = mk_fec_ilv(rec, 6, 0, 2, 2, A0, B0); fec_rx_feed(&f, rec, L, 1000);   /* column 0 */
    L = mk_fec_ilv(rec, 6, 1, 2, 2, A1, B1); fec_rx_feed(&f, rec, L, 1000);   /* column 1 */
    fec_rx_tick(&f, 1000);
    EXPECT(g_rn==4, "interleaved pair emits all 4 data symbols");
    EXPECT(PROTO_SEQ_FROM_FLAGS(g_rec[0][0])==0, "emit order starts block_A SEQ0");
    EXPECT(PROTO_SEQ_FROM_FLAGS(g_rec[3][0])==3, "emit order ends block_B SEQ3");
}

/* Eviction-collapse (cold start: nothing emitted yet, 3 slots full + 1 free).
 * slot_for(bidA) parks bidA in the free slot; slot_for(bidB) then finds all full
 * and evicts the oldest -- which is bidA's just-parked slot (bidA older than the
 * three pre-fills) -> sa==sb. Must drop+count (drops_sanity), never a bad emit or
 * a leaked ILV write. (With the IMP-4 stale guard, the old 4-full/after-emit
 * scenario is caught earlier by the stale path, so cold-start is what still
 * reaches this guard.) Reachable only under forged/misframed block_ids. */
static void test_ilv_eviction_collapse_caught(void){
    reset();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    /* Open THREE blocks (ids 20,21,22), each k=2 n=2 with only idx0 present
     * (have=1 < n=2 -> stays OPEN). One slot stays free; nothing emits yet. */
    uint8_t s0[FEC_SYMBOL_LEN];
    uint8_t pl[4]={9,9,9,9}; mk_inner(s0, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), pl, 4);
    uint8_t rec[600]; size_t L;
    for (uint8_t bid=20; bid<=22; bid++){ L=mk_fec(rec,bid,0,2,2,s0); fec_rx_feed(&f,rec,L,1000); }
    EXPECT(g_rn==0, "pre-fills stay open (no emit)");
    uint32_t sanity_before = f.drops_sanity;
    /* ILV block_A=5 (older than all three), column 0, k=2 n=2. slot_for(5) parks
     * in the free slot; slot_for(6) evicts the now-oldest slot (5) -> sa==sb. */
    uint8_t A0[FEC_SYMBOL_LEN],B0[FEC_SYMBOL_LEN];
    pl[0]=1; mk_inner(A0, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), pl, 4);
    pl[0]=2; mk_inner(B0, PROTO_FLAG_START|PROTO_FLAG_SEQ(1), pl, 4);
    L = mk_fec_ilv(rec, 5, 0, 2, 2, A0, B0); fec_rx_feed(&f, rec, L, 1000);
    EXPECT(f.drops_sanity == sanity_before + 1, "ILV slot-collapse dropped+counted");
    /* No slot may hold bidA=5 (it was overwritten to bidB=6 before any store), and
     * the surviving collapsed slot carries NO ILV symbol (have==0). */
    int leaked = 0;
    for (int i=0;i<FEC_RX_SLOTS;i++){
        if (f.slot[i].used && f.slot[i].block_id == 5) leaked = 1;          /* bidA never persists */
        if (f.slot[i].used && f.slot[i].block_id == 6 && f.slot[i].have) leaked = 1; /* no ILV write */
    }
    EXPECT(!leaked, "no ILV symbol written into the collapsed slot");
}

static void test_two_legacy_blocks_ordered(void){
    reset();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    /* block 5: two inner data symbols */
    uint8_t a0[FEC_SYMBOL_LEN], a1[FEC_SYMBOL_LEN];
    uint8_t pa0[4]={10,11,12,13}, pa1[4]={20,21,22,23};
    mk_inner(a0, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), pa0, 4);
    mk_inner(a1, PROTO_FLAG_END|PROTO_FLAG_SEQ(1),   pa1, 4);
    uint8_t rec[600]; size_t L;
    L = mk_fec_pair(rec, 5, 0, 2, 2, a0, a1); fec_rx_feed(&f, rec, L, 1000);
    /* block 6: two inner data symbols */
    uint8_t b0[FEC_SYMBOL_LEN], b1[FEC_SYMBOL_LEN];
    uint8_t pb0[4]={30,31,32,33}, pb1[4]={40,41,42,43};
    mk_inner(b0, PROTO_FLAG_START|PROTO_FLAG_SEQ(2), pb0, 4);
    mk_inner(b1, PROTO_FLAG_END|PROTO_FLAG_SEQ(3),   pb1, 4);
    L = mk_fec_pair(rec, 6, 0, 2, 2, b0, b1); fec_rx_feed(&f, rec, L, 2000);
    fec_rx_tick(&f, 2000);
    EXPECT(g_rn==4, "both blocks' 4 data symbols emitted");
    EXPECT(g_rec[0][0]&PROTO_FLAG_START, "first emit is block5 sym0 (START)");
    EXPECT(PROTO_SEQ_FROM_FLAGS(g_rec[3][0])==3, "last emit is block6 sym1 (SEQ 3)");
}

/* A forged FEC header with k > FEC_K_MAX must be rejected by the sanity gate,
 * never indexing past the sym[] array. count=1 keeps the payload legal. */
static void test_hostile_oversize_k(void){
    reset();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t sym[FEC_SYMBOL_LEN]; uint8_t p[2]={1,2};
    mk_inner(sym, PROTO_FLAG_START|PROTO_FLAG_END|PROTO_FLAG_SEQ(0), p, 2);
    /* k=210 (>FEC_K_MAX=200), n=k (valid vs n<=k+M and base_idx+count<=n), base_idx=0.
     * The k>FEC_K_MAX sanity gate (which precedes the length walk) rejects it
     * before any per-symbol length read or array index. */
    uint8_t rec[600];
    size_t L = mk_fec(rec, 6, 0, 210, 210, sym);
    uint32_t before = f.drops_sanity;
    fec_rx_feed(&f, rec, L, 1000);
    EXPECT(f.drops_sanity==before+1, "forged k>FEC_K_MAX -> drops_sanity++");
    EXPECT(g_rn==0, "forged oversized-k packet emits nothing");
}

/* A younger block (id7) completes fully while the older (id6, k=3 n=3 m=0) has a
 * permanent gap -> id6 must still emit FIRST (survivors), then id7. Ordered flush. */
static void test_ordered_flush_younger_first(void){
    reset();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t s[FEC_SYMBOL_LEN]; uint8_t p[2];
    uint8_t rec[600]; size_t L;
    /* block 6: k=3 m=0 (n=3), feed only idx0 and idx2 -> gap at idx1, unrecoverable.
     * Each on-wire symbol is a full FEC_SYMBOL_LEN (mk_inner zero-pads s). */
    p[0]=60; mk_inner(s, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), p, 2);
    L = mk_fec(rec, 6, 0, 3, 3, s); fec_rx_feed(&f, rec, L, 1000);
    p[0]=62; mk_inner(s, PROTO_FLAG_END|PROTO_FLAG_SEQ(2), p, 2);
    L = mk_fec(rec, 6, 2, 3, 3, s); fec_rx_feed(&f, rec, L, 1000);
    /* block 7 fully arrives (k=1 m=0) */
    p[0]=70; mk_inner(s, PROTO_FLAG_START|PROTO_FLAG_END|PROTO_FLAG_SEQ(3), p, 2);
    L = mk_fec(rec, 7, 0, 1, 1, s); fec_rx_feed(&f, rec, L, 1000);
    EXPECT(g_rn==0, "block7 complete but held: block6 (older) still open -> nothing emitted");
    fec_rx_tick(&f, 1000ull + FEC_RX_TIMEOUT_NS);   /* time out block6 */
    EXPECT(g_rn>=1, "after block6 times out, flush proceeds in order");
    EXPECT(PROTO_SEQ_FROM_FLAGS(g_rec[0][0])==0, "block6 survivors emit before block7");
}

/* A forged ILV packet with column >= n must drop+count and touch no slot. */
static void test_ilv_hostile_column(void){
    reset();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t A[FEC_SYMBOL_LEN], B[FEC_SYMBOL_LEN]; uint8_t p[2]={1,2};
    mk_inner(A, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), p, 2);
    mk_inner(B, PROTO_FLAG_END|PROTO_FLAG_SEQ(1), p, 2);
    uint8_t rec[600];
    size_t L = mk_fec_ilv(rec, 6, 99 /*col>=n*/, 2, 2, A, B);
    uint32_t before = f.drops_sanity;
    fec_rx_feed(&f, rec, L, 1000);
    EXPECT(f.drops_sanity==before+1, "forged ILV column>=n -> drops_sanity++");
    EXPECT(g_rn==0, "forged ILV touches no slot, emits nothing");
}

/* The length walk's bound + exact-total guards: each malformed packet drops
 * (drops_len), feeds the decoder nothing, and leaves no slot mutated. */
static void test_walk_rejects_malformed(void){
    fec_gf_init();
    uint8_t d0[FEC_SYMBOL_LEN]; uint8_t pl[10]={1,2,3,4,5,6,7,8,9,10};
    size_t d0_inner = mk_inner(d0, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), pl, 10); /* inner=13 */

    /* (a) count=1 data symbol + trailing bytes -> sum < plen -> reject. */
    {
        reset(); FecRx f; fec_rx_init(&f, rec_emit, NULL);
        uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
        proto_fec_pair_hdr_pack(payload, 7, 0, 1, 1, 1);          /* k=1 n=1 idx0 data */
        memcpy(payload+FEC_HDR_BYTES, d0, d0_inner);
        size_t plen = FEC_HDR_BYTES + d0_inner + 5;              /* 5 trailing bytes */
        uint8_t rec[512];
        int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_FEC, payload, plen);
        fec_rx_feed(&f, rec, (size_t)len, 1000);
        EXPECT(f.drops_len==1, "over-long count=1 (trailing) dropped");
        EXPECT(no_slot_open(&f), "no slot mutated on over-long");
    }
    /* (b) count=2, second symbol's inner header runs past plen -> reject. */
    {
        reset(); FecRx f; fec_rx_init(&f, rec_emit, NULL);
        uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
        proto_fec_pair_hdr_pack(payload, 7, 0, 2, 2, 2);          /* k=2 n=2 both data */
        memcpy(payload+FEC_HDR_BYTES, d0, d0_inner);
        size_t plen = FEC_HDR_BYTES + d0_inner + 2;              /* only 2 B for s1 header */
        uint8_t rec[512];
        int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_FEC, payload, plen);
        fec_rx_feed(&f, rec, (size_t)len, 1000);
        EXPECT(f.drops_len==1, "under-long count=2 (s1 header straddles plen) dropped");
        EXPECT(no_slot_open(&f), "no slot mutated on under-long");
    }
    /* (c) data symbol whose inner len field claims 300 -> sl=303>252 -> reject. */
    {
        reset(); FecRx f; fec_rx_init(&f, rec_emit, NULL);
        uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
        proto_fec_pair_hdr_pack(payload, 7, 0, 1, 1, 1);
        payload[FEC_HDR_BYTES] = PROTO_FLAG_START|PROTO_FLAG_END;
        pack_le16(&payload[FEC_HDR_BYTES+1], 300);              /* sl = 303 > 252 */
        size_t plen = FEC_HDR_BYTES + 64;                       /* enough bytes present */
        uint8_t rec[512];
        int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_FEC, payload, plen);
        fec_rx_feed(&f, rec, (size_t)len, 1000);
        EXPECT(f.drops_len==1, "sl>252 dropped");
        EXPECT(no_slot_open(&f), "no slot mutated on sl>252");
    }
}

/* Drop-before-mutate: a count=2 packet whose FIRST symbol is valid but SECOND is
 * malformed must reject with NO partial write of symbol 0 into the slot. */
static void test_walk_no_partial_write(void){
    reset(); fec_gf_init();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t d0[FEC_SYMBOL_LEN]; uint8_t pl[10]={1,2,3,4,5,6,7,8,9,10};
    size_t d0_inner = mk_inner(d0, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), pl, 10);
    uint8_t payload[FEC_HDR_BYTES + FEC_SYMBOL_LEN];
    proto_fec_pair_hdr_pack(payload, 20, 0, 2, 2, 2);            /* k=2 n=2 both data */
    memcpy(payload+FEC_HDR_BYTES, d0, d0_inner);                 /* s0 valid */
    pack_le16(&payload[FEC_HDR_BYTES + d0_inner + 1], 300);      /* s1 inner len 300 -> sl>252 */
    size_t plen = FEC_HDR_BYTES + d0_inner + 16;
    uint8_t rec[512];
    int len = protocol_build(rec, sizeof rec, PROTO_MODE_FIXED, PROTO_FLAG_FEC, payload, plen);
    fec_rx_feed(&f, rec, (size_t)len, 1000);
    EXPECT(f.drops_len==1, "count=2 with bad s1 dropped");
    EXPECT(no_slot_open(&f), "s0 NOT written into a slot (validate-before-write)");
}

/* IMP-4 guard: after a block is emitted (here an m=0 block, which closes on
 * data-complete today), a LATE symbol for that already-emitted bid must be
 * discarded (late_after_emit++), NOT opened as a ghost slot. A genuinely newer
 * bid must still open+emit normally (the guard must not over-reject). */
static void test_stale_bid_guard_no_ghost(void){
    reset();
    fec_gf_init();
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t a0[FEC_SYMBOL_LEN], a1[FEC_SYMBOL_LEN];
    uint8_t p0[4]={1,2,3,4}, p1[4]={5,6,7,8};
    mk_inner(a0, PROTO_FLAG_START|PROTO_FLAG_SEQ(0), p0, 4);
    mk_inner(a1, PROTO_FLAG_END|PROTO_FLAG_SEQ(1),   p1, 4);
    uint8_t rec[600]; size_t L;
    /* m=0 block (k=2 n=2) completes + emits immediately on the pair. */
    L = mk_fec_pair(rec, 5, 0, 2, 2, a0, a1); fec_rx_feed(&f, rec, L, 1000);
    EXPECT(g_rn==2, "m=0 block emits on complete");
    EXPECT(no_slot_open(&f), "slot freed after emit");
    /* Late symbol for the already-emitted bid 5: discard, no ghost slot. */
    L = mk_fec(rec, 5, 0, 2, 2, a0); fec_rx_feed(&f, rec, L, 2000);
    EXPECT(g_rn==2, "late symbol for emitted bid produced no new emit");
    EXPECT(f.late_after_emit==1, "late symbol counted as late_after_emit");
    EXPECT(no_slot_open(&f), "no ghost slot opened for the already-emitted bid");
    /* A genuinely newer bid still opens + emits normally. */
    uint8_t b0[FEC_SYMBOL_LEN], b1[FEC_SYMBOL_LEN];
    uint8_t q0[4]={9,10,11,12}, q1[4]={13,14,15,16};
    mk_inner(b0, PROTO_FLAG_START|PROTO_FLAG_SEQ(2), q0, 4);
    mk_inner(b1, PROTO_FLAG_END|PROTO_FLAG_SEQ(3),   q1, 4);
    L = mk_fec_pair(rec, 6, 0, 2, 2, b0, b1); fec_rx_feed(&f, rec, L, 3000);
    EXPECT(g_rn==4, "newer bid 6 emits normally");
    EXPECT(f.late_after_emit==1, "newer bid not counted as late");
}

/* IMP-4 core: a k=3 n=5 (m=2) block whose 3 DATA symbols all arrive must emit
 * IMMEDIATELY -- no parity, no tick -- via slot_emit's all-data fast path (no RS
 * decode). On current code the slot waits at have<n and emits nothing. */
static void test_early_emit_all_data_no_wait(void){
    reset();
    fec_gf_init();
    uint8_t data[3][FEC_SYMBOL_LEN]; uint16_t dlen[3];
    for(int i=0;i<3;i++){
        uint8_t pl[100]; for(int j=0;j<100;j++) pl[j]=(uint8_t)(i*100+j);
        uint8_t flags=PROTO_FLAG_SEQ(i); if(i==0)flags|=PROTO_FLAG_START; if(i==2)flags|=PROTO_FLAG_END;
        dlen[i]=(uint16_t)mk_inner(data[i],flags,pl,100);
    }
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t rec[512];
    fec_rx_feed(&f, rec, mk_fec(rec,7,0,3,5,data[0]), 1000);
    fec_rx_feed(&f, rec, mk_fec(rec,7,1,3,5,data[1]), 1000);
    fec_rx_feed(&f, rec, mk_fec(rec,7,2,3,5,data[2]), 1000);  /* data complete -> emit now */
    EXPECT(g_rn==3, "all 3 data emitted immediately on data-complete (no tick)");
    EXPECT(f.fec_recovered==0, "fast path: no RS decode performed");
    EXPECT(f.fec_blocks==1, "exactly one block closed+emitted");
    EXPECT(no_slot_open(&f), "slot freed after early emit");
    EXPECT((g_rec[0][0]&PROTO_FLAG_START) && g_rlen[0]==dlen[0], "order: idx0 first (START)");
    EXPECT(PROTO_SEQ_FROM_FLAGS(g_rec[2][0])==2, "order: idx2 last");
}

/* IMP-4 caveat: after early emit, the block's trailing parity symbols arrive
 * late for an already-emitted bid -> discarded by the guard, no ghost slot, no
 * double emit, no spurious decode. */
static void test_late_parity_discarded_no_ghost(void){
    reset();
    fec_gf_init();
    uint8_t data[3][FEC_SYMBOL_LEN]; uint16_t dlen[3];
    for(int i=0;i<3;i++){
        uint8_t pl[100]; for(int j=0;j<100;j++) pl[j]=(uint8_t)(i*100+j);
        uint8_t flags=PROTO_FLAG_SEQ(i); if(i==0)flags|=PROTO_FLAG_START; if(i==2)flags|=PROTO_FLAG_END;
        dlen[i]=(uint16_t)mk_inner(data[i],flags,pl,100);
    }
    uint8_t par[2][FEC_SYMBOL_LEN]; const uint8_t *dp[3]; uint8_t *pp[2];
    for(int i=0;i<3;i++) dp[i]=data[i];
    for(int i=0;i<2;i++) pp[i]=par[i];
    fec_encode(dp,3,2,pp);
    FecRx f; fec_rx_init(&f, rec_emit, NULL);
    uint8_t rec[512];
    fec_rx_feed(&f, rec, mk_fec(rec,7,0,3,5,data[0]), 1000);
    fec_rx_feed(&f, rec, mk_fec(rec,7,1,3,5,data[1]), 1000);
    fec_rx_feed(&f, rec, mk_fec(rec,7,2,3,5,data[2]), 1000);   /* early emit */
    EXPECT(g_rn==3, "block early-emitted on data-complete");
    /* Late parity for the already-emitted bid 7. */
    fec_rx_feed(&f, rec, mk_fec(rec,7,3,3,5,par[0]), 2000);
    fec_rx_feed(&f, rec, mk_fec(rec,7,4,3,5,par[1]), 2000);
    EXPECT(g_rn==3, "late parity produced no new emit");
    EXPECT(f.late_after_emit==2, "two late parity symbols counted");
    EXPECT(no_slot_open(&f), "no ghost slot opened for the emitted bid");
    EXPECT(f.fec_recovered==0 && f.fec_unrecoverable==0, "no spurious decode/unrecoverable");
}

int main(void){
    test_passthrough_fec0();
    test_recover_one_loss();
    test_unrecoverable_emits_survivors();
    test_packed_pair_recovers_loss();
    test_rx_validation_drops();
    test_fixed_frame_511_no_oob();
    test_inner_len_overrange_rejected();
    test_ilv_pair_assembles();
    test_ilv_eviction_collapse_caught();
    test_two_legacy_blocks_ordered();
    test_hostile_oversize_k();
    test_ordered_flush_younger_first();
    test_ilv_hostile_column();
    test_walk_rejects_malformed();
    test_walk_no_partial_write();
    test_stale_bid_guard_no_ghost();
    test_early_emit_all_data_no_wait();
    test_late_parity_discarded_no_ghost();
    if(g_failures){fprintf(stderr,"test_fec_rx: %d failure(s)\n",g_failures);return 1;}
    printf("test_fec_rx: OK\n"); return 0;
}
