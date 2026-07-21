#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/lr2021/lr2021_status.h"

/* Stat(15:0) per datasheet Table 6-38: CommandStatus in bits 11:9 (high
 * byte bits 3:1), ChipMode in bits 2:0 (low byte). */
static void test_cmd_status(void) {
    /* CommandStatus = 0x2 (CMD_OK) -> bits 11:9 = 010 -> high byte bit 2 set
     * (0x04). Other bits (e.g. bit15:12=0) don't matter. */
    assert(lr2021_stat_cmd_status(0x04) == LR2021_CMD_OK);
    assert(!lr2021_stat_cmd_failed(0x04));

    /* CMD_DAT = 0x3 -> bits = 011 -> high byte bits 1,2 -> 0x06. Success. */
    assert(lr2021_stat_cmd_status(0x06) == LR2021_CMD_DAT);
    assert(!lr2021_stat_cmd_failed(0x06));

    /* CMD_FAIL = 0x0 -> high-byte command bits clear -> 0x00. Failure. */
    assert(lr2021_stat_cmd_status(0x00) == LR2021_CMD_FAIL);
    assert(lr2021_stat_cmd_failed(0x00));

    /* CMD_PERR = 0x1 -> bits = 001 -> high byte bit 1 -> 0x02. Failure. */
    assert(lr2021_stat_cmd_status(0x02) == LR2021_CMD_PERR);
    assert(lr2021_stat_cmd_failed(0x02));

    /* InterruptStatus (Stat bit 8 = high-byte bit 0) must not bleed into
     * CommandStatus: high byte 0x05 = CMD_OK(0x04) + IntStatus(0x01) -> OK. */
    assert(lr2021_stat_cmd_status(0x05) == LR2021_CMD_OK);
}

static void test_chip_mode(void) {
    assert(lr2021_stat_chip_mode(0x01) == LR2021_CHIPMODE_STDBY_RC);
    assert(lr2021_stat_chip_mode(0x03) == LR2021_CHIPMODE_FS);
    assert(lr2021_stat_chip_mode(0x04) == LR2021_CHIPMODE_RX);
    assert(lr2021_stat_chip_mode(0x05) == LR2021_CHIPMODE_TX);
    /* Upper bits (ResetSource 7:4, rfu 3) must not leak into ChipMode. */
    assert(lr2021_stat_chip_mode(0xF5) == LR2021_CHIPMODE_TX);
}

static void test_errors(void) {
    __u8 e[2] = { 0x00, 0x00 };
    assert(lr2021_decode_errors(e) == 0);
    /* PLL_LOCK_ERR (bit 2) -> ErrorStat = 0x0004 -> hi=0x00 lo=0x04. */
    e[0] = 0x00; e[1] = 0x04;
    assert(lr2021_decode_errors(e) == LR2021_ERR_PLL_LOCK);
    /* PA_OFFSET_CALIB_ERR (bit 11) -> 0x0800 -> hi=0x08 lo=0x00. */
    e[0] = 0x08; e[1] = 0x00;
    assert(lr2021_decode_errors(e) == LR2021_ERR_PA_OFFSET_CALIB);
    /* HF_XOSC_START (bit0) + CHIP_BUSY (bit8) -> 0x0101. */
    e[0] = 0x01; e[1] = 0x01;
    assert(lr2021_decode_errors(e) == (LR2021_ERR_HF_XOSC_START | LR2021_ERR_CHIP_BUSY));
}

static void test_flrc_rx_stats(void) {
    /* pkt_rx=0x1234, crc=0x0056, len=0x0007, all MSB-first. */
    __u8 in[6] = { 0x12, 0x34, 0x00, 0x56, 0x00, 0x07 };
    struct lr2021_flrc_rx_stats s;
    memset(&s, 0xAA, sizeof(s));
    lr2021_decode_flrc_rx_stats(in, &s);
    assert(s.pkt_rx == 0x1234);
    assert(s.crc_error == 0x0056);
    assert(s.len_error == 0x0007);
}

int main(void) {
    test_cmd_status();
    test_chip_mode();
    test_errors();
    test_flrc_rx_stats();
    printf("All status decoder tests passed!\n");
    return 0;
}
