#include "../apps/pivid-tx/src/fec_class.h"
#include <stdio.h>

static int g_failures = 0;
#define EXPECT(c,m) do{ if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,m);g_failures++;} }while(0)

int main(void) {
    /* nal_unit_type is the low 5 bits; nal_ref_idc is bits 5-6 (0x60). */
    EXPECT(fec_nal_classify(0x65) == FEC_NAL_IDR,  "type 5 (ref_idc=3) -> IDR");
    EXPECT(fec_nal_classify(0x25) == FEC_NAL_IDR,  "type 5 (ref_idc=1) -> IDR");
    EXPECT(fec_nal_classify(0x67) == FEC_NAL_SPS,  "type 7 -> SPS");
    EXPECT(fec_nal_classify(0x68) == FEC_NAL_PPS,  "type 8 -> PPS");
    EXPECT(fec_nal_classify(0x61) == FEC_NAL_REF,  "type 1 ref_idc=3 -> REF");
    EXPECT(fec_nal_classify(0x41) == FEC_NAL_REF,  "type 1 ref_idc=2 -> REF");
    EXPECT(fec_nal_classify(0x01) == FEC_NAL_DISP, "type 1 ref_idc=0 -> DISP");

    /* m mapping: IDR/SPS/PPS -> anchor; REF -> ref; DISP -> passthrough (<0). */
    EXPECT(fec_class_m(FEC_NAL_IDR, 5, 0) == 5,  "IDR uses m_anchor");
    EXPECT(fec_class_m(FEC_NAL_SPS, 5, 0) == 5,  "SPS uses m_anchor");
    EXPECT(fec_class_m(FEC_NAL_PPS, 5, 0) == 5,  "PPS uses m_anchor");
    EXPECT(fec_class_m(FEC_NAL_REF, 5, 0) == 0,  "REF uses m_ref");
    EXPECT(fec_class_m(FEC_NAL_REF, 5, 1) == 1,  "REF uses m_ref (=1)");
    EXPECT(fec_class_m(FEC_NAL_DISP, 5, 1) < 0,  "DISP is passthrough");

    if (!g_failures) printf("OK\n");
    return g_failures ? 1 : 0;
}
