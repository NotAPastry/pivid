#include "sps_reinject.h"
#include <string.h>

/* H.264 nal_unit_type values (nal[0] & 0x1F). */
#define NAL_TYPE_NON_IDR_SLICE 1
#define NAL_TYPE_IDR_SLICE     5
#define NAL_TYPE_SPS           7
#define NAL_TYPE_PPS           8

void sps_reinject_init(SpsReinject *r, uint64_t interval_ns) {
    memset(r, 0, sizeof *r);
    r->interval_ns = interval_ns;
}

void sps_reinject_on_nal(SpsReinject *r, const uint8_t *nal, size_t len,
                         uint64_t now_ns, SpsReinjectEmit emit, void *user) {
    if (len == 0) return;
    uint8_t type = nal[0] & 0x1F;

    if (type == NAL_TYPE_SPS) {
        if (len <= SPS_REINJECT_PARAM_MAX) {
            memcpy(r->sps, nal, len);
            r->sps_len = len;
            r->have_sps = 1;
        } else {
            r->have_sps = 0;          /* too big to cache; don't re-inject a truncated set */
        }
        r->last_param_ns = now_ns;    /* the real SPS passes through, so params just went out */
        r->primed = 1;
        return;
    }
    if (type == NAL_TYPE_PPS) {
        if (len <= SPS_REINJECT_PARAM_MAX) {
            memcpy(r->pps, nal, len);
            r->pps_len = len;
            r->have_pps = 1;
        } else {
            r->have_pps = 0;
        }
        r->last_param_ns = now_ns;
        r->primed = 1;
        return;
    }

    if (type == NAL_TYPE_NON_IDR_SLICE || type == NAL_TYPE_IDR_SLICE) {
        if (r->interval_ns != 0 && r->have_sps && r->have_pps && r->primed &&
            now_ns >= r->last_param_ns &&
            (now_ns - r->last_param_ns) >= r->interval_ns) {
            emit(r->sps, r->sps_len, user);
            emit(r->pps, r->pps_len, user);
            r->last_param_ns = now_ns;
            r->reinjections++;
        }
    }
}
