#ifndef LINK_CAPACITY_H
#define LINK_CAPACITY_H

#include <stdint.h>

/* Predict the over-the-air payload ceiling and the usable-video budget for the
 * current link config, calibrated to measured 511 B throughput.
 *
 * Two numbers, both in kbps (kilobits/sec), matching the live tx_kbps/rx_kbps
 * convention:
 *   *air_kbps   -- the over-the-air payload ceiling the live Mbps stat (which
 *                  counts video + software-FEC parity bytes) approaches at
 *                  saturation. Set by chip FEC (flrc_cr) + the packet model.
 *                  Software FEC does NOT change it.
 *   *video_kbps -- air_kbps * k/(k+m): the slice left for actual video after
 *                  software FEC. This is what the encoder bitrate must stay under.
 *
 * Model (full-packet, back-to-back ceiling):
 *   us_per_byte = 4.10 * (0.75 / cr_frac)      cr3/4 -> 4.10, cr1/1 -> 3.075
 *   ov_per_pkt  = 303 us  (framing + serial-loop host idle, CALIBRATED to the
 *                          measured 511 B anchor after the 2026-07-05 IMP-2
 *                          campaign: TXPERF period 2446 us at saturation ->
 *                          cr3/4 ceiling ~1.67 Mbps; cr1/1 predicted ~2.13 Mbps.
 *                          Pre-IMP-2 this was 846 us / ~1.39 Mbps. Recalibrate
 *                          whenever the SPI hot path changes: anchor = TXPERF
 *                          `period` from a TXPERF driver build.)
 *   dyn_extra   = 48 us (DYNAMIC SetFlrcPacketParams per-packet reprogram,
 *                        always added; included in the 2446 us anchor)
 *   PLD         = 511 (DYNAMIC full-packet ceiling)
 *
 * The 4.10 us/byte airtime constant is shared with pkt_hist.c's US_PER_BYTE.
 * The 846 us overhead here is deliberately larger than pkt_hist's FRAMING_US
 * (400): pkt_hist compares RELATIVE fixed-vs-dynamic airtime (host idle
 * cancels); this model must match ABSOLUTE measured throughput (host idle
 * counts).
 *
 * DYNAMIC is the only framing, so the result is the saturated full-packet
 * ceiling. Dynamic's real advantage -- efficiency on sub-full packets -- shows
 * up in the live stat, not the ceiling.
 *
 * All inputs are guarded: invalid flrc_cr falls back to cr 3/4, fec_k is forced
 * >=1 and fec_m >=0, so the result is always a finite, non-negative number
 * (never divide-by-zero, never NaN). flrc_pld_len is accepted but ignored
 * (dynamic always saturates the full 511 B packet).
 *
 * flrc_cr:   2 = 1/1 (no chip FEC); anything else = 3/4 (default).
 */
void link_capacity(int flrc_cr, int flrc_pld_len,
                   int fec_m, int fec_k,
                   uint64_t *air_kbps, uint64_t *video_kbps);

#endif /* LINK_CAPACITY_H */
