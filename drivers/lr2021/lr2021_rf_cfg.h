/* Pure RF byte encoders shared by the kernel bring-up (lr2021_init.c) and
 * host tests. Uses <linux/types.h> (__u8/__u32/__u64) so it compiles in both
 * kernel space and userspace (glibc provides <linux/types.h> too -- same
 * pattern as lr2021_flrc_cfg.h). All values cross-referenced to the LR2021
 * V2.1 datasheet sections quoted in lr2021_init.c. Host-tested in
 * tests/test_rf_cfg.c. */
#ifndef LR2021_RF_CFG_H
#define LR2021_RF_CFG_H

#include <linux/types.h>   /* __u8 / __u32 / __u64 -- kernel + host (libc) */

/* CalibFE (6.4.2) is issued at bring-up with NO frequency arguments (see
 * lr2021_init.c step 7): the chip self-selects the calibration slot by
 * truncating the current RF frequency down to a 4 MHz multiple -- the same
 * convention SetRx uses to look it up -- so there is no host-side slot encode
 * to get wrong. The configured carrier is snapped to that 4 MHz grid upstream
 * (snap_rf_freq_to_calib_grid in common/settings.c) so the cal lands exactly on
 * the carrier. An earlier explicit round-to-nearest Freq1 encoder lived here;
 * removed with the no-arg switch (see DEEP_RESULTS.md BUG-1). */

/* SetRfFrequency (7.3.1): 4 bytes rf_freq in Hz, MSB first. */
static inline void lr2021_encode_rf_freq(__u64 freq_hz, __u8 out[4])
{
    __u32 hz = (__u32)freq_hz;
    out[0] = (__u8)(hz >> 24);
    out[1] = (__u8)(hz >> 16);
    out[2] = (__u8)(hz >>  8);
    out[3] = (__u8)(hz & 0xFFu);
}

/* SetTxParams (7.4.3): byte0 = tx_power as signed int8 in 0.5 dB steps, HF
 * range [-39:24] -> [-19.5:+12] dBm; byte1 = PA ramp index per table 7-21
 * (0x00=2 us .. 0x12=304 us; 0x0A=128 us, 0x03=16 us). dbm is clamped to the
 * HF PA range; ramp_idx is clamped to the table. */
static inline void lr2021_encode_tx_params(int dbm, unsigned int ramp_idx,
                                           __u8 out[2])
{
    int steps = dbm * 2;             /* 0.5 dB steps */
    if (steps > 24)  steps = 24;     /* +12 dBm HF max */
    if (steps < -39) steps = -39;    /* -19.5 dBm HF min */
    if (ramp_idx > 0x12u) ramp_idx = 0x12u;   /* last table 7-21 row (304 us) */
    out[0] = (__u8)(__s8)steps;
    out[1] = (__u8)ramp_idx;
}

#endif /* LR2021_RF_CFG_H */
