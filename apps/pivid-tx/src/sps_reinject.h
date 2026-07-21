#ifndef TX_SPS_REINJECT_H
#define TX_SPS_REINJECT_H

#include <stddef.h>
#include <stdint.h>

/* SPS/PPS are tiny (well under 100 B for H.264 Baseline); 256 is ample.
 * A param set larger than this is not cached (won't re-inject a truncated set). */
#define SPS_REINJECT_PARAM_MAX 256

typedef void (*SpsReinjectEmit)(const uint8_t *nal, size_t len, void *user);

/* Periodic SPS/PPS re-injection for intra-refresh (GDR) streams.
 *
 * With cyclic intra-refresh + i_period=0 the encoder emits SPS/PPS only at the
 * single startup IDR. On the one-way link, an RX that loses that IDR (fade,
 * late join) can never re-acquire the parameter sets and the video dies. This
 * caches the SPS/PPS as they pass and re-emits them before a coded slice once
 * interval_ns has elapsed since they were last sent. */
typedef struct {
    uint64_t interval_ns;                     /* 0 disables re-injection */
    uint8_t  sps[SPS_REINJECT_PARAM_MAX];
    size_t   sps_len;
    uint8_t  pps[SPS_REINJECT_PARAM_MAX];
    size_t   pps_len;
    int      have_sps;
    int      have_pps;
    int      primed;                          /* last_param_ns is valid */
    uint64_t last_param_ns;                   /* when SPS/PPS were last sent */
    uint64_t reinjections;                    /* stat: re-inject events */
} SpsReinject;

void sps_reinject_init(SpsReinject *r, uint64_t interval_ns);

/* Call for each outgoing NAL BEFORE the caller emits it. Caches SPS (type 7)
 * and PPS (type 8) and resets the timer when they pass. On a coded slice
 * (type 1 or 5), if both are cached and >= interval_ns has elapsed, emits the
 * cached SPS then PPS via `emit` and resets the timer. The caller always emits
 * `nal` itself afterward. */
void sps_reinject_on_nal(SpsReinject *r, const uint8_t *nal, size_t len,
                         uint64_t now_ns, SpsReinjectEmit emit, void *user);

#endif
