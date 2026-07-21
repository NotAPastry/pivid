#define _POSIX_C_SOURCE 200809L

#include "../apps/pivid-tx/src/aqm.h"
#include "../apps/pivid-tx/src/tx.h"
#include "../common/ring_user.h"
#include "../common/radio_ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define EXPECT(c,m) do{ if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,m);g_failures++;} }while(0)

/* NAL header byte helpers: ref_idc in bits 6-5, nut in bits 4-0. */
#define NALH(ref_idc,nut) ((uint8_t)(((ref_idc)<<5) | (nut)))

static void test_is_vcl(void){
    EXPECT(aqm_is_vcl(NALH(3,1)), "type 1 (non-IDR slice) is VCL");
    EXPECT(aqm_is_vcl(NALH(3,5)), "type 5 (IDR slice) is VCL");
    EXPECT(aqm_is_vcl(NALH(0,2)), "type 2 (data partition) treated as VCL");
    EXPECT(!aqm_is_vcl(NALH(3,7)), "type 7 (SPS) not VCL");
    EXPECT(!aqm_is_vcl(NALH(3,8)), "type 8 (PPS) not VCL");
    EXPECT(!aqm_is_vcl(NALH(0,6)), "type 6 (SEI) not VCL");
    EXPECT(!aqm_is_vcl(NALH(0,9)), "type 9 (AUD) not VCL");
    EXPECT(!aqm_is_vcl(NALH(0,0)), "type 0 (unspecified) not VCL");
}

static void test_au_start(void){
    /* first_mb_in_slice==0 -> first RBSP byte top bit set (ue(v)==0). */
    uint8_t au0[2]   = { NALH(3,1), 0x88 };   /* 0x88 top bit set -> first_mb==0 */
    uint8_t cont[2]  = { NALH(3,1), 0x40 };   /* top bit clear -> first_mb!=0 (continuation) */
    uint8_t idr0[2]  = { NALH(3,5), 0x90 };   /* IDR, first_mb==0 */
    uint8_t sps[2]   = { NALH(3,7), 0xFF };   /* non-VCL, never an AU start here */
    uint8_t shortn[1]= { NALH(3,1) };         /* len<2 guard */
    EXPECT(aqm_is_au_start(au0,  2), "VCL + first_mb==0 -> AU start");
    EXPECT(!aqm_is_au_start(cont, 2), "VCL + first_mb!=0 -> continuation, not AU start");
    EXPECT(aqm_is_au_start(idr0, 2), "IDR + first_mb==0 -> AU start");
    EXPECT(!aqm_is_au_start(sps, 2), "non-VCL -> not AU start");
    EXPECT(!aqm_is_au_start(shortn, 1), "len<2 -> not AU start (guard)");
}

static void test_classify(void){
    uint8_t idr[2] = { NALH(3,5), 0x80 };
    uint8_t ref[2] = { NALH(2,1), 0x80 };
    uint8_t dis[2] = { NALH(0,1), 0x80 };
    EXPECT(aqm_classify(idr,2)==AQM_CLASS_IDR,  "nut==5 -> IDR");
    EXPECT(aqm_classify(ref,2)==AQM_CLASS_REF,  "ref_idc!=0, nut!=5 -> REF");
    EXPECT(aqm_classify(dis,2)==AQM_CLASS_DISP, "ref_idc==0 -> DISP");
}

static void test_hysteresis(void){
    /* watermarks: disp_hi=40, disp_lo=25, ref_hi=55, ref_lo=45 */
    AqmLevel s = AQM_LVL_NONE;
    s = aqm_hysteresis_step(s, 30, 40,25,55,45); EXPECT(s==AQM_LVL_NONE, "30% stays NONE");
    s = aqm_hysteresis_step(s, 42, 40,25,55,45); EXPECT(s==AQM_LVL_DISP, "42% -> DISPOSABLE");
    s = aqm_hysteresis_step(s, 38, 40,25,55,45); EXPECT(s==AQM_LVL_DISP, "38% holds DISPOSABLE (hysteresis, >disp_lo)");
    s = aqm_hysteresis_step(s, 20, 40,25,55,45); EXPECT(s==AQM_LVL_NONE, "20% < disp_lo -> NONE");
    s = aqm_hysteresis_step(s, 60, 40,25,55,45); EXPECT(s==AQM_LVL_REF, "60% -> REF (skips straight up)");
    s = aqm_hysteresis_step(s, 50, 40,25,55,45); EXPECT(s==AQM_LVL_REF, "50% holds REF (>ref_lo)");
    s = aqm_hysteresis_step(s, 44, 40,25,55,45); EXPECT(s==AQM_LVL_DISP, "44% < ref_lo -> DISPOSABLE");
    s = aqm_hysteresis_step(s, 10, 40,25,55,45); EXPECT(s==AQM_LVL_NONE, "10% collapses REF->...->NONE");
    /* No oscillation: a value parked between disp_lo and disp_hi keeps state. */
    AqmLevel n = AQM_LVL_NONE;
    n = aqm_hysteresis_step(n, 35, 40,25,55,45); EXPECT(n==AQM_LVL_NONE, "35% never entered -> stays NONE");
}

static void test_policy_table(void){
    /* IDR is NEVER dropped under any recipe/level. */
    for (int r=0; r<AQM_RCP_COUNT; r++){
        EXPECT(!aqm_should_drop((AqmRecipe)r, AQM_LVL_REF, AQM_CLASS_IDR), "IDR never dropped");
    }
    /* droppable is graded: DISP drops at DISPOSABLE, REF only at REF. */
    EXPECT(aqm_should_drop(AQM_RCP_DROPPABLE, AQM_LVL_DISP, AQM_CLASS_DISP), "droppable: DISP drops at DISPOSABLE");
    EXPECT(!aqm_should_drop(AQM_RCP_DROPPABLE, AQM_LVL_DISP, AQM_CLASS_REF), "droppable: REF held at DISPOSABLE");
    EXPECT(aqm_should_drop(AQM_RCP_DROPPABLE, AQM_LVL_REF, AQM_CLASS_REF), "droppable: REF drops at REF");
    /* gdr/gop drop any non-IDR at DISPOSABLE. */
    EXPECT(aqm_should_drop(AQM_RCP_GDR, AQM_LVL_DISP, AQM_CLASS_REF), "gdr: P drops at DISPOSABLE");
    EXPECT(aqm_should_drop(AQM_RCP_GOP, AQM_LVL_DISP, AQM_CLASS_REF), "gop: P drops at DISPOSABLE");
    /* NONE level never drops anything. */
    EXPECT(!aqm_should_drop(AQM_RCP_GDR, AQM_LVL_NONE, AQM_CLASS_REF), "level NONE never drops");
    /* fallback == gop for every (level,class). */
    for (int l=0; l<=AQM_LVL_REF; l++)
        for (int c=0; c<3; c++)
            EXPECT(aqm_should_drop(AQM_RCP_FALLBACK,(AqmLevel)l,(AqmClass)c)
                   == aqm_should_drop(AQM_RCP_GOP,(AqmLevel)l,(AqmClass)c),
                   "fallback behaves identically to gop");
}

static void test_parse_helpers(void){
    EXPECT(aqm_parse_recipe("gdr")==AQM_RCP_GDR, "recipe gdr");
    EXPECT(aqm_parse_recipe("droppable")==AQM_RCP_DROPPABLE, "recipe droppable");
    EXPECT(aqm_parse_recipe("gop")==AQM_RCP_GOP, "recipe gop");
    EXPECT(aqm_parse_recipe(NULL)==AQM_RCP_FALLBACK, "recipe unset -> fallback");
    EXPECT(aqm_parse_recipe("weird")==AQM_RCP_FALLBACK, "recipe unknown -> fallback");
    EXPECT(aqm_parse_signal("peak")==AQM_SIG_PEAK, "signal peak");
    EXPECT(aqm_parse_signal("inst")==AQM_SIG_INST, "signal inst");
    EXPECT(aqm_parse_signal("ewma")==AQM_SIG_EWMA, "signal ewma");
    EXPECT(aqm_parse_signal("res")==AQM_SIG_RES, "signal res");
    EXPECT(aqm_parse_signal(NULL)==AQM_SIG_PEAK, "signal unset -> peak");
}

static void test_aqm_config_no_env(void) {
    unsetenv("AQM"); unsetenv("AQM_RECIPE");
    AqmConfig ac = { .mode = AQM_OFF, .recipe = AQM_RCP_DROPPABLE };
    struct TX_Dev dev; memset(&dev, 0, sizeof dev);
    Aqm a;
    EXPECT(aqm_start(&a, &dev, &ac) == 0, "aqm_start with cfg returns 0");
    EXPECT(a.mode == AQM_OFF, "mode from cfg, not env");
    EXPECT(a.recipe == AQM_RCP_DROPPABLE, "recipe from cfg, not env");
    aqm_stop(&a, &dev);
}

static void test_recipe_from_encoder(void) {
    EXPECT(aqm_recipe_from_encoder("gdr")     == AQM_RCP_GDR,       "gdr -> GDR");
    EXPECT(aqm_recipe_from_encoder("droppable")== AQM_RCP_DROPPABLE, "droppable -> DROPPABLE");
    EXPECT(aqm_recipe_from_encoder("gop")     == AQM_RCP_GOP,       "gop -> GOP");
    EXPECT(aqm_recipe_from_encoder("gdr_idr") == AQM_RCP_GOP,       "gdr_idr -> GOP (policy map)");
    EXPECT(aqm_recipe_from_encoder("weird")   == AQM_RCP_FALLBACK,  "unknown -> FALLBACK");
    EXPECT(aqm_recipe_from_encoder(NULL)      == AQM_RCP_FALLBACK,  "NULL -> FALLBACK");
}

static void test_start_off(void){
    /* OFF: no CSV, hooks wired, defaults applied. dev with NULL ring is fine. */
    struct TX_Dev devbuf; memset(&devbuf, 0, sizeof devbuf);
    AqmConfig ac = { .mode = AQM_OFF, .recipe = AQM_RCP_FALLBACK };
    Aqm a;
    int rc = aqm_start(&a, &devbuf, &ac);
    EXPECT(rc==0, "aqm_start OFF returns 0");
    EXPECT(a.mode==AQM_OFF, "OFF mode");
    EXPECT(a.csv==NULL, "OFF opens no CSV");
    EXPECT(a.t_disp_hi==40 && a.t_disp_lo==25 && a.t_ref_hi==55 && a.t_ref_lo==45,
           "default watermarks");
    aqm_stop(&a, &devbuf);
}

static void test_start_enforce_env(void){
    struct TX_Dev devbuf; memset(&devbuf, 0, sizeof devbuf);
    setenv("PIVID_TX_SAMPLE", "1", 1);
    setenv("PIVID_AQM_SIGNAL", "inst", 1);
    setenv("PIVID_AQM_T_DISP_HIGH", "33", 1);
    setenv("PIVID_TX_SAMPLE_CSV", "/tmp/test_aqm_enforce.csv", 1);
    remove("/tmp/test_aqm_enforce.csv");
    AqmConfig ac = { .mode = AQM_ENFORCE, .recipe = AQM_RCP_DROPPABLE };
    Aqm a;
    int rc = aqm_start(&a, &devbuf, &ac);
    EXPECT(rc==0, "aqm_start ENFORCE returns 0");
    EXPECT(a.mode==AQM_ENFORCE, "ENFORCE mode");
    EXPECT(a.recipe==AQM_RCP_DROPPABLE, "recipe parsed");
    EXPECT(a.signal==AQM_SIG_INST, "signal parsed");
    EXPECT(a.t_disp_hi==33, "watermark override parsed");
    EXPECT(a.csv!=NULL, "sampler flag opened the shared CSV");
    aqm_stop(&a, &devbuf);
    /* clean env for later tests */
    unsetenv("PIVID_TX_SAMPLE");
    unsetenv("PIVID_AQM_SIGNAL"); unsetenv("PIVID_AQM_T_DISP_HIGH");
    unsetenv("PIVID_TX_SAMPLE_CSV");
}

/* Build a fake ring: a zeroed RADIO_RING_BYTES blob whose wr/rd we poke to
 * synthesize an occupancy. set_occ(pct) sets used = pct% of the data region. */
static void *fake_ring(void){ return calloc(1, RADIO_RING_BYTES); }
static void set_occ(void *ring, unsigned pct){
    uint32_t used = (uint32_t)((uint64_t)RADIO_RING_DATA_BYTES * pct / 100u);
    *ring_rd_ptr(ring) = 0;
    *ring_wr_ptr(ring) = used;
}

static void test_filter_off_keeps_all(void){
    struct TX_Dev d; memset(&d,0,sizeof d);
    AqmConfig ac = { .mode = AQM_OFF, .recipe = AQM_RCP_FALLBACK };
    Aqm a; aqm_start(&a,&d,&ac);
    uint8_t idr[2]={NALH(3,5),0x80};
    EXPECT(aqm_filter_nal(&a,idr,2)==1, "OFF keeps VCL");
    EXPECT(aqm_filter_nal(&a,(uint8_t[]){NALH(3,7),0xFF},2)==1, "OFF keeps non-VCL");
    aqm_stop(&a,&d);
}


static void test_filter_enforce_latch(void){
    void *ring = fake_ring(); set_occ(ring, 90);
    struct TX_Dev d; memset(&d,0,sizeof d); d.tx_ring=ring;
    setenv("PIVID_TX_SAMPLE","1",1);
    setenv("PIVID_AQM_SIGNAL","inst",1);
    setenv("PIVID_TX_SAMPLE_CSV","/tmp/test_aqm_enf.csv",1);
    AqmConfig ac = { .mode = AQM_ENFORCE, .recipe = AQM_RCP_GOP };
    Aqm a; aqm_start(&a,&d,&ac);

    /* P frame at 90% -> dropped. Multi-slice: AU-start + continuation both gone. */
    uint8_t p_start[2]={NALH(3,1),0x80};     /* first_mb==0 */
    uint8_t p_cont [2]={NALH(3,1),0x40};     /* first_mb!=0 continuation */
    uint8_t sei    [2]={NALH(0,6),0xFF};     /* non-VCL between AUs */
    EXPECT(aqm_filter_nal(&a,p_start,2)==0, "ENFORCE drops P-frame AU start");
    EXPECT(aqm_filter_nal(&a,p_cont, 2)==0, "latch drops the continuation slice");
    EXPECT(aqm_filter_nal(&a,sei,    2)==1, "non-VCL always kept (even mid-drop)");
    EXPECT(a.nal_suppressed==2, "two VCL NALs suppressed");

    /* Now IDR at 90% -> never dropped. */
    uint8_t idr[2]={NALH(3,5),0x80};
    EXPECT(aqm_filter_nal(&a,idr,2)==1, "ENFORCE never drops IDR");
    EXPECT(a.au_kept[AQM_CLASS_IDR]==1, "IDR kept counted");

    /* Drop ring to 0% -> next P frame kept (hysteresis fell to NONE). */
    set_occ(ring, 0);
    uint8_t p2[2]={NALH(3,1),0x80};
    EXPECT(aqm_filter_nal(&a,p2,2)==1, "P frame kept once occupancy collapses");
    aqm_stop(&a,&d);
    free(ring);
    unsetenv("PIVID_TX_SAMPLE"); unsetenv("PIVID_AQM_SIGNAL");
    unsetenv("PIVID_TX_SAMPLE_CSV");
}

static void test_filter_sps_pps_never_dropped(void){
    /* Regression guard: SPS/PPS are non-VCL, so the line-173 VCL guard keeps
     * them BEFORE the dropping_au suppression gate can ever see them. Even with
     * the latch set at a REF-dropping level under droppable, parameter sets
     * survive -- they can never be stranded ahead of the IDR they precede. */
    void *ring = fake_ring(); set_occ(ring, 90);          /* >= t_ref_hi=55 -> REF */
    struct TX_Dev d; memset(&d,0,sizeof d); d.tx_ring=ring;
    setenv("PIVID_AQM_SIGNAL","inst",1);                  /* read ring occupancy, not peak */
    AqmConfig ac = { .mode = AQM_ENFORCE, .recipe = AQM_RCP_DROPPABLE };
    Aqm a; aqm_start(&a,&d,&ac);

    /* REF P-frame AU start at 90% -> droppable drops REF at REF level -> latch set. */
    uint8_t pref[2]={NALH(2,1),0x80};
    EXPECT(aqm_filter_nal(&a,pref,2)==0, "droppable drops a REF P-frame at REF level");
    EXPECT(a.dropping_au==1, "dropping_au latched");

    /* With the latch set, SPS then PPS must still be KEPT. */
    EXPECT(aqm_filter_nal(&a,(uint8_t[]){NALH(3,7),0xFF},2)==1, "SPS kept while dropping_au latched");
    EXPECT(aqm_filter_nal(&a,(uint8_t[]){NALH(3,8),0xFF},2)==1, "PPS kept while dropping_au latched");
    EXPECT(a.nal_suppressed==1, "only the P slice suppressed; SPS/PPS not counted");
    aqm_stop(&a,&d);
    free(ring);
    unsetenv("PIVID_AQM_SIGNAL");
}

static void test_peak_tracking(void){
    void *ring = fake_ring();
    struct TX_Dev d; memset(&d,0,sizeof d); d.tx_ring=ring;
    setenv("PIVID_TX_SAMPLE","1",1);
    setenv("PIVID_AQM_SIGNAL","peak",1);
    setenv("PIVID_AQM_T_DISP_HIGH","40",1);
    setenv("PIVID_TX_SAMPLE_CSV","/tmp/test_aqm_peak.csv",1);
    AqmConfig ac = { .mode = AQM_ENFORCE, .recipe = AQM_RCP_GDR };
    Aqm a; aqm_start(&a,&d,&ac);

    /* Simulate a frame's pushes spiking to 60% then draining: peak must win. */
    uint32_t hi = (uint32_t)((uint64_t)RADIO_RING_DATA_BYTES*60/100);
    aqm_on_push(&a, hi, 0);                 /* peak <- 60% */
    aqm_on_push(&a, hi/4, 0);               /* lower; peak stays 60% */
    set_occ(ring, 5);                        /* instantaneous reads 5% (trough) */
    uint8_t p[2]={NALH(3,1),0x80};
    /* peak signal sees 60% >= 40% -> drop; inst would have read 5% and kept. */
    EXPECT(aqm_filter_nal(&a,p,2)==0, "peak trips the gate -> AU suppressed");
    EXPECT(a.au_dropped[AQM_CLASS_REF]==1, "peak (60%) tripped the gate despite 5% instantaneous");
    EXPECT(__atomic_load_n(&a.peak_used,__ATOMIC_RELAXED)==0, "peak reset to 0 after the boundary");
    aqm_stop(&a,&d);
    free(ring);
    unsetenv("PIVID_TX_SAMPLE"); unsetenv("PIVID_AQM_SIGNAL");
    unsetenv("PIVID_AQM_T_DISP_HIGH");
    unsetenv("PIVID_TX_SAMPLE_CSV");
}

static void test_enforce_without_sample_no_csv(void){
    void *ring = fake_ring(); set_occ(ring, 90);
    struct TX_Dev d; memset(&d,0,sizeof d); d.tx_ring=ring;
    unsetenv("PIVID_TX_SAMPLE");                 /* logging OFF */
    setenv("PIVID_AQM_SIGNAL","inst",1);
    AqmConfig ac = { .mode = AQM_ENFORCE, .recipe = AQM_RCP_DROPPABLE };
    Aqm a; int rc = aqm_start(&a,&d,&ac);
    EXPECT(rc==0, "enforce without sampler returns 0 (not disabled)");
    EXPECT(a.mode==AQM_ENFORCE, "still ENFORCE without the sampler flag");
    EXPECT(a.csv==NULL, "no CSV acquired when PIVID_TX_SAMPLE unset");
    uint8_t disp[2]={NALH(0,1),0x80};           /* disposable P, AU start, ring 90% */
    EXPECT(aqm_filter_nal(&a,disp,2)==0, "enforce still suppresses without logging");
    EXPECT(a.nal_suppressed==1, "one VCL NAL suppressed (latch works sans CSV)");
    aqm_stop(&a,&d);
    free(ring);
    unsetenv("PIVID_AQM_SIGNAL");
}

int main(void){
    test_is_vcl();
    test_au_start();
    test_classify();
    test_hysteresis();
    test_policy_table();
    test_parse_helpers();
    test_recipe_from_encoder();
    test_aqm_config_no_env();
    test_start_off();
    test_start_enforce_env();
    test_filter_off_keeps_all();
    test_filter_enforce_latch();
    test_filter_sps_pps_never_dropped();
    test_enforce_without_sample_no_csv();
    test_peak_tracking();
    if(g_failures){fprintf(stderr,"test_aqm: %d failure(s)\n",g_failures);return 1;}
    printf("test_aqm: OK\n"); return 0;
}
