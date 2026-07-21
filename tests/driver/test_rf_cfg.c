#include "../drivers/lr2021/lr2021_rf_cfg.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint8_t b4[4], tp[2];

    /* CalibFE is now issued no-arg at bring-up (the chip self-selects the slot),
     * so there is no host-side CalibFE encoder to test. See DEEP_RESULTS.md BUG-1.
     * The 4 MHz-grid snap that keeps the carrier cal-aligned is tested against
     * snap_rf_freq_to_calib_grid() in the settings unit tests. */

    /* SetRfFrequency: 4 bytes big-endian Hz. 2028000000 = 0x78E0D300 (the
     * 4 MHz-grid-aligned S-band carrier). */
    lr2021_encode_rf_freq(2028000000ull, b4);
    assert(b4[0] == 0x78 && b4[1] == 0xE0 && b4[2] == 0xD3 && b4[3] == 0x00);

    /* SetTxParams byte0 = power*2 (0.5 dB steps), clamped to [-39,24];
     * byte1 = ramp index (table 7-21), clamped to [0x00,0x12]. */
    lr2021_encode_tx_params(12, 0x0A, tp);
    assert(tp[0] == 24 && tp[1] == 0x0A);
    lr2021_encode_tx_params(12, 0x03, tp);      /* 16 us ramp (IMP-2a) */
    assert(tp[0] == 24 && tp[1] == 0x03);
    lr2021_encode_tx_params(0, 0x00, tp);       /* fastest table row, 2 us */
    assert((int8_t)tp[0] == 0 && tp[1] == 0x00);
    lr2021_encode_tx_params(99, 0x0A, tp);      /* over HF max -> clamp +12 dBm */
    assert((int8_t)tp[0] == 24);
    lr2021_encode_tx_params(-100, 0x0A, tp);    /* under min -> clamp -19.5 dBm */
    assert((int8_t)tp[0] == -39);
    lr2021_encode_tx_params(12, 0xFF, tp);      /* off-table ramp -> clamp 0x12 */
    assert(tp[1] == 0x12);
    printf("test_rf_cfg: OK\n");
    return 0;
}
