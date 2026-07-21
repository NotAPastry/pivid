#include "link_capacity.h"

/* Physical airtime constant, shared with pkt_hist.c's US_PER_BYTE: us per
 * payload byte after CR 3/4 chip FEC. */
#define US_PER_BYTE_CR34   4.10
/* Per-packet overhead (us): framing + real serial-loop host idle. Calibrated
 * so the model reproduces the measured 511 B anchor: TXPERF period 2446 us
 * after the 2026-07-05 IMP-2 campaign (fast BUSY-wait + 16 us PA ramp) ->
 * 2446 - 511*4.10 - DYN_EXTRA ~= 303. Was 846 pre-IMP-2 (anchor 1.39 Mbps);
 * still intentionally larger than pkt_hist's FRAMING_US (400 predates IMP-2
 * but only feeds a RELATIVE fixed-vs-dynamic comparison) -- see the header. */
#define OV_PER_PKT_US      303.0
/* Dynamic mode adds a per-packet SetFlrcPacketParams reprogram (us). */
#define DYN_EXTRA_US       48.0

#define PLD_MAX   511

static uint64_t round_u64(double v) {
    if (v <= 0.0) return 0;
    return (uint64_t)(v + 0.5);
}

void link_capacity(int flrc_cr, int flrc_pld_len,
                   int fec_m, int fec_k,
                   uint64_t *air_kbps, uint64_t *video_kbps) {
    (void)flrc_pld_len;   /* dynamic always saturates the full 511 B packet */

    /* Chip FEC: only flrc_cr==2 means 1/1 (no chip FEC); anything else --
     * including 0, 3, or negatives -- falls back to the conservative 3/4. */
    double cr_frac = (flrc_cr == 2) ? 1.00 : 0.75;
    double us_per_byte = US_PER_BYTE_CR34 * (0.75 / cr_frac);

    /* DYNAMIC is the only framing: the per-packet ceiling is the full 511 B
     * packet and dynamic carries its on-air length overhead. */
    int pld = PLD_MAX;

    double ov = OV_PER_PKT_US + DYN_EXTRA_US;
    double us_per_pkt = (double)pld * us_per_byte + ov;

    /* kbps = bytes*8 (bits) / (us/1000) (ms) == bytes*8*1000 / us. */
    double air = ((double)pld * 8.0 * 1000.0) / us_per_pkt;

    /* Software FEC carves video out of the air ceiling. Guard the divisor:
     * k>=1 and m>=0 => k+m>=1, never divide-by-zero. */
    int k = (fec_k < 1) ? 1 : fec_k;
    int m = (fec_m < 0) ? 0 : fec_m;
    double video = (m == 0) ? air : air * (double)k / (double)(k + m);

    if (air_kbps)   *air_kbps = round_u64(air);
    if (video_kbps) *video_kbps = round_u64(video);
}
