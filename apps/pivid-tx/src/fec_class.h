#ifndef TX_FEC_CLASS_H
#define TX_FEC_CLASS_H

#include <stdint.h>

/* NAL class for per-class FEC allocation. IDR/SPS/PPS are the GOP anchors
 * (heavy parity); REF is a reference P-frame; DISP is a disposable
 * (non-reference) frame. The class boundaries here were validated against the
 * offline FEC simulation during the per-class FEC bring-up (the sim harness has
 * since been retired; see test_fec_class.c for the live coverage). */
typedef enum {
    FEC_NAL_IDR = 0,   /* nal_unit_type 5            */
    FEC_NAL_SPS,       /* nal_unit_type 7            */
    FEC_NAL_PPS,       /* nal_unit_type 8            */
    FEC_NAL_REF,       /* nal_ref_idc != 0, not the above */
    FEC_NAL_DISP,      /* nal_ref_idc == 0           */
} FecNalClass;

/* Classify by the NAL header byte (Annex-B start code already stripped;
 * nal0 is the byte whose low 5 bits are nal_unit_type, bits 5-6 nal_ref_idc).
 * The caller must pass a non-empty NAL; this function reads nal0 unconditionally. */
static inline FecNalClass fec_nal_classify(uint8_t nal0) {
    unsigned type = nal0 & 0x1F;
    if (type == 5) return FEC_NAL_IDR;
    if (type == 7) return FEC_NAL_SPS;
    if (type == 8) return FEC_NAL_PPS;
    if (nal0 & 0x60) return FEC_NAL_REF;
    return FEC_NAL_DISP;
}

/* Parity (m) for a class given the two production knobs. IDR/SPS/PPS -> anchor;
 * REF -> ref; DISP -> passthrough (negative => plain, no FEC). */
static inline int fec_class_m(FecNalClass c, int m_anchor, int m_ref) {
    switch (c) {
        case FEC_NAL_IDR:
        case FEC_NAL_SPS:
        case FEC_NAL_PPS: return m_anchor;
        case FEC_NAL_REF: return m_ref;
        case FEC_NAL_DISP: return -1;
    }
    return -1;   /* unreachable: caller passed a bad enum value */
}

/* Block-grouping key: NALs with the same key may share one RS block. SPS and PPS
 * share GRP_PARAMSET (one anchor parity set instead of two); IDR is its own group
 * so it stays a clean multi-block interleave unit. (Parity m is still chosen by
 * fec_class_m; this key only decides block boundaries.) */
typedef enum { GRP_IDR = 0, GRP_PARAMSET, GRP_REF, GRP_DISP } FecGroupKey;

static inline FecGroupKey fec_group_key(FecNalClass c) {
    switch (c) {
        case FEC_NAL_IDR:               return GRP_IDR;
        case FEC_NAL_SPS:
        case FEC_NAL_PPS:               return GRP_PARAMSET;
        case FEC_NAL_REF:               return GRP_REF;
        case FEC_NAL_DISP:              return GRP_DISP;
    }
    return GRP_DISP;   /* unreachable */
}

#endif /* TX_FEC_CLASS_H */
