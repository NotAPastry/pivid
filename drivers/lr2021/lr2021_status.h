#ifndef LR2021_STATUS_H
#define LR2021_STATUS_H

#include <linux/types.h>   /* __u8 / __u16 -- kernel + host (libc) */

/* Pure decoders for the LR2021's status / diagnostic command responses.
 *
 * Mirrors lr2021_flrc_cfg.h: no SPI, no locks, static-inline only, so the
 * exact byte/bit layout is shared verbatim by the driver and the host unit
 * test (tests/test_status.c). All offsets are cross-referenced against the
 * LR2021/LR2022/LR2012 V2.1 datasheet (text dump at
 * docs/datasheets/pdftotext/62785538.LR20xxDatasheet_V2_1.txt) with the
 * section/table quoted at each helper.
 */

/* --- Stat(15:0) -- returned at the start of every SPI frame ------------- *
 *
 * Datasheet Table 6-38 "Status (Stat) Definition" (dump ~16094):
 *   bits (15:12) = 0x0 (fixed)
 *   bits (11:9)  = CommandStatus
 *   bit  (8)     = InterruptStatus
 *   bits (7:4)   = ResetSource
 *   bit  (3)     = rfu
 *   bits (2:0)   = ChipMode
 *
 * The host receives Stat MSB-first: byte 0 = Stat(15:8), byte 1 = Stat(7:0).
 * CommandStatus therefore lives in the HIGH byte (bits 11:9 = high-byte bits
 * 3:1); ChipMode lives in the LOW byte (bits 2:0). */

/* CommandStatus values (datasheet 16123-16127). */
#define LR2021_CMD_FAIL  0x0u  /* latest command could not be executed       */
#define LR2021_CMD_PERR  0x1u  /* latest command not processed (bad op/args) */
#define LR2021_CMD_OK    0x2u  /* latest command processed successfully      */
#define LR2021_CMD_DAT   0x3u  /* successful read; data follows IrqStatus    */

/* ChipMode values (datasheet 16137-16143). */
#define LR2021_CHIPMODE_SLEEP      0x0u
#define LR2021_CHIPMODE_STDBY_RC   0x1u
#define LR2021_CHIPMODE_STDBY_XOSC 0x2u
#define LR2021_CHIPMODE_FS         0x3u
#define LR2021_CHIPMODE_RX         0x4u
#define LR2021_CHIPMODE_TX         0x5u

static inline __u8 lr2021_stat_cmd_status(__u8 stat_hi) {
    return (__u8)((stat_hi >> 1) & 0x07u);   /* Stat bits 11:9 */
}

static inline __u8 lr2021_stat_chip_mode(__u8 stat_lo) {
    return (__u8)(stat_lo & 0x07u);          /* Stat bits 2:0 */
}

/* True iff CommandStatus reports the previous command failed/was rejected.
 * OK (0x2) and DAT (0x3 -- successful read) are the success codes. */
static inline int lr2021_stat_cmd_failed(__u8 stat_hi) {
    __u8 cs = lr2021_stat_cmd_status(stat_hi);
    return (cs == LR2021_CMD_FAIL) || (cs == LR2021_CMD_PERR);
}

/* --- GetErrors (0x0110) ErrorStat(15:0) -------------------------------- *
 *
 * Response after the 2 Stat bytes is ErrorStat(15:8) then ErrorStat(7:0)
 * (datasheet Table 6-43, dump ~16320). Bit meanings (dump 16278+):
 *   0 HF_XOSC_START_ERR  1 LF_XOSC_START_ERR  2 PLL_LOCK_ERR
 *   3 LF_RC_CALIB_ERR    4 HF_RC_CALIB_ERR     5 PLL_CALIB_ERR
 *   6 AAF_CALIB_ERR      7 IMG_CALIB_ERR       8 CHIP_BUSY
 *   9 RXFREQ_NO_FE_CAL_ERR 10 MEAS_UNIT_ADC_CALIB_ERR 11 PA_OFFSET_CALIB_ERR
 */
#define LR2021_ERR_HF_XOSC_START   (1u << 0)
#define LR2021_ERR_LF_XOSC_START   (1u << 1)
#define LR2021_ERR_PLL_LOCK        (1u << 2)
#define LR2021_ERR_LF_RC_CALIB     (1u << 3)
#define LR2021_ERR_HF_RC_CALIB     (1u << 4)
#define LR2021_ERR_PLL_CALIB       (1u << 5)
#define LR2021_ERR_AAF_CALIB       (1u << 6)
#define LR2021_ERR_IMG_CALIB       (1u << 7)
#define LR2021_ERR_CHIP_BUSY       (1u << 8)
#define LR2021_ERR_RXFREQ_NO_FE_CAL (1u << 9)
#define LR2021_ERR_MEAS_ADC_CALIB  (1u << 10)
#define LR2021_ERR_PA_OFFSET_CALIB (1u << 11)

/* err[0] = ErrorStat(15:8), err[1] = ErrorStat(7:0). */
static inline __u16 lr2021_decode_errors(const __u8 err[2]) {
    return (__u16)(((__u16)err[0] << 8) | (__u16)err[1]);
}

/* --- GetFlrcRxStats (0x024A) ------------------------------------------- *
 *
 * Chip-side FLRC reception statistics -- ground truth independent of the
 * host's framing. Response after the 2 Stat bytes is six bytes, each field
 * 16-bit MSB-first (datasheet Table 18-7, dump ~26996):
 *   [0..1] pkt_rx    total received packets
 *   [2..3] crc_error received with CRC error
 *   [4..5] len_error received with length error
 * Reset on POR / sleep-without-retention / ResetRxStats (dump 27028). */
struct lr2021_flrc_rx_stats {
    __u16 pkt_rx;
    __u16 crc_error;
    __u16 len_error;
};

static inline void lr2021_decode_flrc_rx_stats(const __u8 in[6],
                                               struct lr2021_flrc_rx_stats *s) {
    s->pkt_rx    = (__u16)(((__u16)in[0] << 8) | (__u16)in[1]);
    s->crc_error = (__u16)(((__u16)in[2] << 8) | (__u16)in[3]);
    s->len_error = (__u16)(((__u16)in[4] << 8) | (__u16)in[5]);
}

#endif /* LR2021_STATUS_H */
