#include "../apps/pivid-tx/src/fec_tx.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define EXPECT(c,m) do{ if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,m);g_failures++;} }while(0)

#define HDROFF 3   /* FIXED-mode outer header is flags(1)+len(2) */

#define CAP 256
static uint8_t g_pkt[CAP][512];
static size_t  g_len[CAP];
static int     g_n;
static void rec_push(const uint8_t *p, size_t n, void *u){ (void)u; if(g_n<CAP){memcpy(g_pkt[g_n],p,n);g_len[g_n]=n;g_n++;} }
static void reset(void){ g_n=0; }

static int count_flag(uint8_t flag){ int c=0; for(int i=0;i<g_n;i++) if(g_pkt[i][0]&flag) c++; return c; }

static void fec_tally(int *out_pkts, int *out_syms, int *out_pairs, int *out_tails){
    int pkts=0,syms=0,pairs=0,tails=0;
    for(int i=0;i<g_n;i++){
        if(!(g_pkt[i][0]&PROTO_FLAG_FEC)) continue;
        uint8_t bid,base,k,n,count;
        proto_fec_pair_hdr_unpack(&g_pkt[i][HDROFF],&bid,&base,&k,&n,&count);
        pkts++; syms+=count; if(count==2)pairs++; else tails++;
    }
    *out_pkts=pkts; *out_syms=syms; *out_pairs=pairs; *out_tails=tails;
}

/* Block dims (k,n) per distinct FEC block_id, in first-seen order. */
static int block_dims(uint8_t *ks, uint8_t *ns, int max){
    int nb=0; int seen[128]={0};
    for(int i=0;i<g_n;i++){
        if(!(g_pkt[i][0]&PROTO_FLAG_FEC)) continue;
        uint8_t bid,base,k,n,count;
        proto_fec_pair_hdr_unpack(&g_pkt[i][HDROFF],&bid,&base,&k,&n,&count);
        if(!seen[bid]){ seen[bid]=1; if(nb<max){ks[nb]=k; ns[nb]=n; nb++;} }
    }
    return nb;
}

/* DISP NAL -> plain passthrough, no FEC. */
static void test_disposable_passthrough(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,0,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t nal[300]; nal[0]=0x01;            /* nal_ref_idc=0 -> DISP */
    for(int i=1;i<300;i++) nal[i]=(uint8_t)i;
    fec_tx_nal(&f,nal,sizeof nal);
    fec_tx_flush(&f);
    EXPECT(g_n==1, "300B disposable -> 1 plain chunk (508 cap)");
    EXPECT(count_flag(PROTO_FLAG_FEC)==0, "disposable has no FEC flag");
}

/* ref-P at m_ref=0 -> plain passthrough (the production default). */
static void test_refp_m0_passthrough(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,0,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t ref[600]; ref[0]=0x61;            /* ref_idc=3 type 1 -> REF */
    for(int i=1;i<600;i++) ref[i]=(uint8_t)(i*5);
    fec_tx_nal(&f,ref,sizeof ref);
    fec_tx_flush(&f);
    EXPECT(count_flag(PROTO_FLAG_FEC)==0, "ref-P at m_ref=0 is plain (no FEC flag)");
    EXPECT(g_n==2, "600B ref-P -> 2 plain chunks (508 cap)");
}

/* ref-P at m_ref=1 -> its own single-class FEC block. */
static void test_refp_m1_block(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,1,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t ref[600]; ref[0]=0x61;
    for(int i=1;i<600;i++) ref[i]=(uint8_t)(i*5);
    fec_tx_nal(&f,ref,sizeof ref);
    fec_tx_flush(&f);
    int pkts,syms,pairs,tails; fec_tally(&pkts,&syms,&pairs,&tails);
    /* 600B -> ceil(600/249)=3 data syms; m_ref=1 -> n=4. */
    EXPECT(syms==4, "ref-P m=1: n=4 symbols (3 data + 1 parity)");
}

/* Anchor (IDR) gets m_anchor. */
static void test_anchor_block_uses_m_anchor(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,0,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t idr[600]; idr[0]=0x65;            /* IDR */
    for(int i=1;i<600;i++) idr[i]=(uint8_t)(i*5);
    fec_tx_nal(&f,idr,sizeof idr);
    fec_tx_flush(&f);
    int pkts,syms,pairs,tails; fec_tally(&pkts,&syms,&pairs,&tails);
    /* 600B -> 3 data syms; m_anchor=5 -> n=8. */
    EXPECT(syms==8, "IDR m_anchor=5: n=8 symbols (3 data + 5 parity)");
}

/* Class change flushes: IDR then ref-P(m=0) -> IDR block closes, ref-P plain. */
static void test_class_change_flushes(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,0,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t idr[300]; idr[0]=0x65; for(int i=1;i<300;i++) idr[i]=(uint8_t)i;
    uint8_t ref[300]; ref[0]=0x61; for(int i=1;i<300;i++) ref[i]=(uint8_t)(i*3);
    fec_tx_nal(&f,idr,sizeof idr);
    fec_tx_nal(&f,ref,sizeof ref);            /* class change -> flush IDR, ref plain */
    fec_tx_flush(&f);
    int pkts,syms,pairs,tails; fec_tally(&pkts,&syms,&pairs,&tails);
    /* IDR: 300B -> 2 data syms; m=5 -> n=7. ref-P plain (no FEC). */
    EXPECT(syms==7, "IDR block has 2 data + 5 parity = 7 symbols");
    /* The plain packet(s) after the FEC block are the ref-P chunks: 300B fits
     * one 508B plain chunk, so exactly one plain packet follows. */
    int plain=0; for(int i=0;i<g_n;i++) if(!(g_pkt[i][0]&PROTO_FLAG_FEC)) plain++;
    EXPECT(plain==1, "ref-P emitted as 1 plain chunk after the IDR block");
}

/* SPS+PPS share GRP_PARAMSET; IDR is its own group -> two blocks. */
static void test_sps_pps_idr_separate_blocks(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,0,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t sps[12]; sps[0]=0x67; for(int i=1;i<12;i++) sps[i]=(uint8_t)i;
    uint8_t pps[8];  pps[0]=0x68; for(int i=1;i<8;i++)  pps[i]=(uint8_t)i;
    uint8_t idr[300];idr[0]=0x65; for(int i=1;i<300;i++)idr[i]=(uint8_t)i;
    fec_tx_nal(&f,sps,sizeof sps);
    fec_tx_nal(&f,pps,sizeof pps);
    fec_tx_nal(&f,idr,sizeof idr);
    fec_tx_flush(&f);
    uint8_t ks[8], ns[8];
    int nb = block_dims(ks, ns, 8);
    EXPECT(nb==2, "SPS+PPS merged paramset block, then a separate IDR block");
    /* Each anchor block carries m_anchor=5 parity: n-k==5 for all. */
    int allm5=1; for(int i=0;i<nb;i++) if(ns[i]-ks[i]!=5) allm5=0;
    EXPECT(allm5, "every anchor block has 5 parity symbols");
}

/* k_bound splits a large anchor into multiple blocks. */
static void test_kbound_split(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,2,0,4,&seq,PROTO_MODE_FIXED,rec_push,NULL); /* K=4 */
    uint8_t idr[2000]; idr[0]=0x65; for(int i=1;i<2000;i++) idr[i]=(uint8_t)(i*5);
    fec_tx_nal(&f,idr,sizeof idr);
    fec_tx_flush(&f);
    EXPECT(f.fec_blocks==3, "3 real RS blocks despite 2 visible block_ids");
    uint8_t ks[16], ns[16];
    int nb = block_dims(ks, ns, 16);
    /* 2000B -> ceil(2000/249)=9 data syms; K=4 -> 3 real blocks (4,4,1). The two
     * full blocks interleave (only block_A's id is on the wire, block_B's id is
     * implicit) + the ragged tail is legacy, so block_dims sees 2 distinct ids,
     * not 3. The 3rd block still exists -- it rides the ILV packets as block_A+1. */
    EXPECT(nb==2, "9 data syms at K=4 -> 1 ILV pair (1 visible id) + 1 legacy tail");
}

/* A NAL at the parser's 65536-byte ceiling cannot be represented by the u16
 * length prefix. It must be dropped + counted, never abort the daemon. */
static void test_oversize_nal_dropped(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,0,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    static uint8_t nal[65536]; nal[0]=0x65;              /* IDR (anchor, would be FEC path) */
    for(int i=1;i<65536;i++) nal[i]=(uint8_t)(i*7);
    fec_tx_nal(&f,nal,sizeof nal);
    fec_tx_flush(&f);
    EXPECT(g_n==0, "65536B NAL produces no wire packets (dropped)");
    EXPECT(f.oversize_drops==1, "oversize_drops counter bumped");
}

/* SPS (type 7) then PPS (type 8) now share ONE k=2 anchor block. */
static void test_sps_pps_merge(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,9,2,16,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t sps[40]; sps[0]=0x67; for(int i=1;i<40;i++) sps[i]=(uint8_t)i;   /* type 7 */
    uint8_t pps[12]; pps[0]=0x68; for(int i=1;i<12;i++) pps[i]=(uint8_t)i;   /* type 8 */
    fec_tx_nal(&f,sps,sizeof sps);
    fec_tx_nal(&f,pps,sizeof pps);
    fec_tx_flush(&f);
    uint8_t ks[128], ns[128];
    int nb = block_dims(ks,ns,128);
    EXPECT(nb==1, "SPS+PPS form a single block");
    EXPECT(ks[0]==2, "merged block has k=2 (one SPS symbol + one PPS symbol)");
    EXPECT(ns[0]==2+9, "merged block n = k + m_anchor(9)");
}

/* An IDR (type 5) following SPS/PPS still flushes to its OWN block group. */
static void test_idr_separate_from_paramsets(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,9,2,16,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t sps[40]; sps[0]=0x67; for(int i=1;i<40;i++) sps[i]=(uint8_t)i;
    uint8_t pps[12]; pps[0]=0x68; for(int i=1;i<12;i++) pps[i]=(uint8_t)i;
    uint8_t idr[200]; idr[0]=0x65; for(int i=1;i<200;i++) idr[i]=(uint8_t)(i*3);  /* type 5 */
    fec_tx_nal(&f,sps,sizeof sps);
    fec_tx_nal(&f,pps,sizeof pps);
    fec_tx_nal(&f,idr,sizeof idr);
    fec_tx_flush(&f);
    uint8_t ks[128], ns[128];
    int nb = block_dims(ks,ns,128);
    EXPECT(nb==2, "SPS+PPS block, then a separate IDR block");
    EXPECT(ks[0]==2, "first block is the merged paramset (k=2)");
}

/* Tally interleaved (ILV) packets and verify each carries column j with the two
 * symbols belonging to block_A and block_A+1. Returns count of ILV packets. */
static int ilv_tally(void){
    int ilv=0;
    for(int i=0;i<g_n;i++){
        if(!(g_pkt[i][0]&PROTO_FLAG_FEC)) continue;
        if(!(g_pkt[i][0]&PROTO_FLAG_FEC_ILV)) continue;
        uint8_t a,col,k,n,count;
        proto_fec_pair_hdr_unpack(&g_pkt[i][HDROFF],&a,&col,&k,&n,&count);
        EXPECT(count==2, "ILV packet is always count=2");
        EXPECT(col < n, "ILV column < n");
        ilv++;
    }
    return ilv;
}

/* A large IDR spans multiple full k_bound=16 blocks -> the full blocks interleave
 * pairwise; the ragged tail falls back to legacy. */
static void test_idr_interleave(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,9,2,16,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    static uint8_t idr[18000]; idr[0]=0x65; for(size_t i=1;i<sizeof idr;i++) idr[i]=(uint8_t)(i*7);
    fec_tx_nal(&f,idr,sizeof idr);
    fec_tx_flush(&f);
    EXPECT(f.fec_blocks==5, "5 real RS blocks (4 full + tail) despite fewer visible ids");
    int ilv = ilv_tally();
    EXPECT(ilv > 0, "multi-block IDR emits interleaved packets");
    uint8_t ks[128], ns[128];
    int nb = block_dims(ks,ns,128);
    /* 5 real blocks (4 full @k_bound=16 + 1 tail), but block_dims only sees
     * block_A ids: the 4 full blocks form 2 ILV pairs (2 visible ids) and the
     * tail is legacy (1 id) -> 3 distinct ids. block_B ids ride the ILV packets
     * implicitly (block_A+1) so they never appear standalone -- expected, do not
     * try to make block_dims see them. fec_blocks==5 is verified elsewhere. */
    EXPECT(nb >= 2, "IDR produced several blocks (block_dims under-counts ILV pairs)");
}

/* A single-block NAL (small ref-P, m_ref>0) never interleaves. */
static void test_single_block_no_interleave(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,9,2,16,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t ref[500]; ref[0]=0x61; for(int i=1;i<500;i++) ref[i]=(uint8_t)(i*5);  /* REF, m_ref=2 */
    fec_tx_nal(&f,ref,sizeof ref);
    fec_tx_flush(&f);
    EXPECT(ilv_tally()==0, "single-block ref-P is never interleaved");
}

/* Helper: real on-wire length of one data symbol embedded at byte offset `o`
 * in packet i (inner FIXED header len at o+1). */
static size_t emb_sym_len(int i, size_t o){ return (size_t)3 + (size_t)unpack_le16(&g_pkt[i][o+1]); } /* 3 = inner FIXED hdr (NOT HDROFF) */

/* A small ref-P block truncates its data symbol: the FEC packet is shorter than
 * the full-width 511 B frame, and its parity tail is still a full 252 B. */
static void test_small_block_truncates(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,1,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t ref[30]; ref[0]=0x61; for(int i=1;i<30;i++) ref[i]=(uint8_t)(i*5);  /* REF, m_ref=1 */
    fec_tx_nal(&f,ref,sizeof ref);
    fec_tx_flush(&f);
    /* 30B -> 1 data sym (idx0) + 1 parity (idx1) -> one count=2 pair. */
    int fec_i=-1; for(int i=0;i<g_n;i++) if(g_pkt[i][0]&PROTO_FLAG_FEC){fec_i=i;break;}
    EXPECT(fec_i>=0, "ref-P m=1 produced a FEC packet");
    uint8_t bid,base,k,n,count; proto_fec_pair_hdr_unpack(&g_pkt[fec_i][HDROFF],&bid,&base,&k,&n,&count);
    EXPECT(count==2 && k==1 && n==2, "one count=2 pair: 1 data + 1 parity");
    EXPECT(g_len[fec_i] < (size_t)(HDROFF + FEC_HDR_BYTES + 2*FEC_SYMBOL_LEN), "packet shorter than 511");
    /* Parity is the 2nd symbol (idx1 >= k=1): the bytes after the truncated data
     * symbol must be a full FEC_SYMBOL_LEN. */
    size_t data_off = (size_t)HDROFF + FEC_HDR_BYTES;
    size_t l0 = emb_sym_len(fec_i, data_off);
    size_t parity_bytes = g_len[fec_i] - (HDROFF + FEC_HDR_BYTES + l0);
    EXPECT(parity_bytes == FEC_SYMBOL_LEN, "parity symbol stays full 252 B");
}

/* A data symbol filled to capacity self-describes as 252, so its packet is NOT
 * shortened -- a genuinely-full data symbol still rides full width. A 250 B NAL
 * is multi-fragment (250 > 249), so its START symbol carries a 2-byte LE total-
 * length prefix: inner payload = 2 + 247 NAL bytes = 249, +3 FIXED header = 252.
 * idx0 (block index 0 < k) is that full data symbol; a 3-byte tail spills into a
 * 2nd symbol, but we only assert the first symbol self-describes as full 252. */
static void test_full_data_symbol_unchanged(void){
    reset();
    uint8_t seq=0; FecTx f; fec_tx_init(&f,5,1,FEC_K_MAX,&seq,PROTO_MODE_FIXED,rec_push,NULL);
    uint8_t ref[1+249]; ref[0]=0x61; for(int i=1;i<250;i++) ref[i]=(uint8_t)(i*7);
    fec_tx_nal(&f,ref,sizeof ref);
    fec_tx_flush(&f);
    int fec_i=-1; for(int i=0;i<g_n;i++) if(g_pkt[i][0]&PROTO_FLAG_FEC){fec_i=i;break;}
    EXPECT(fec_i>=0, "produced a FEC packet");
    size_t data_off = (size_t)HDROFF + FEC_HDR_BYTES;
    EXPECT(emb_sym_len(fec_i, data_off) == FEC_SYMBOL_LEN, "full data symbol self-describes as 252");
}

int main(void){
    fec_gf_init();
    test_disposable_passthrough();
    test_refp_m0_passthrough();
    test_refp_m1_block();
    test_anchor_block_uses_m_anchor();
    test_class_change_flushes();
    test_sps_pps_idr_separate_blocks();
    test_kbound_split();
    test_oversize_nal_dropped();
    test_sps_pps_merge();
    test_idr_separate_from_paramsets();
    test_idr_interleave();
    test_single_block_no_interleave();
    test_small_block_truncates();
    test_full_data_symbol_unchanged();
    if(!g_failures) printf("OK\n");
    return g_failures?1:0;
}
