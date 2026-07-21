#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../common/radio_ioctl.h"
#include "../drivers/lr2021/lr2021_flrc_cfg.h"

/* Build a config the encoder helper consumes. The ioctl struct carries the
 * raw datasheet field codes (preamble_len/sync_len are the 4-bit/2-bit field
 * VALUES per table 18-5, not byte counts) so userspace owns the full range. */
static struct radio_flrc_config base_fixed(void) {
    struct radio_flrc_config c;
    memset(&c, 0, sizeof(c));
    c.mode = 1;          /* fixed */
    c.pld_len = 511;
    c.cr = 1;            /* 3/4 */
    c.crc_width = 1;     /* 16-bit (crc field code 1) */
    c.preamble_len = 7;  /* agc_pbl_len=7 -> 32-bit */
    c.sync_len = 2;      /* sync_len=2 -> 32-bit */
    c.tx_low_threshold = 192;
    return c;
}

static void test_fixed_511(void) {
    /* out[0] = (7<<2)|2 = 0x1E ; out[1] = (1<<6)|(1<<3)|(1<<2)|1 = 0x4D
     * out[2..3] = 0x01,0xFF (511). */
    struct radio_flrc_config c = base_fixed();
    __u8 out[4] = {0};
    lr2021_flrc_encode_params(&c, out);
    assert(out[0] == 0x1E);
    assert(out[1] == 0x4D);
    assert(out[2] == 0x01);
    assert(out[3] == 0xFF);
}

static void test_dynamic_clears_pktfmt(void) {
    /* mode=0 (dynamic): byte1 bit2 must be 0 -> 0x4D becomes 0x49. */
    struct radio_flrc_config c = base_fixed();
    c.mode = 0;
    __u8 out[4] = {0};
    lr2021_flrc_encode_params(&c, out);
    assert(out[1] == 0x49);
}

static void test_crc_width_codes(void) {
    /* IMP-5: all four datasheet Crc codes (table 18-5: 0=OFF 1=16 2=24 3=32
     * bit) land in byte1 bits[1:0] without disturbing the pinned
     * sync_tx/sync_match bits. Dynamic mode: byte1 = 0x48 | crc. */
    struct radio_flrc_config c = base_fixed();
    c.mode = 0;
    __u8 out[4] = {0};
    unsigned int code;
    for (code = 0; code <= 3; code++) {
        c.crc_width = (__u8)code;
        lr2021_flrc_encode_params(&c, out);
        assert(out[1] == (0x48 | code));
    }
}

static void test_full_pld_len_range(void) {
    struct radio_flrc_config c = base_fixed();
    c.pld_len = 6;
    __u8 out[4] = {0};
    lr2021_flrc_encode_params(&c, out);
    assert(out[2] == 0x00 && out[3] == 0x06);
    c.pld_len = 511;
    lr2021_flrc_encode_params(&c, out);
    assert(out[2] == 0x01 && out[3] == 0xFF);
}

static void test_field_masking(void) {
    /* Out-of-range field codes must be masked, never bleed into neighbours.
     * sync_tx/sync_match are NOT struct fields -- they are pinned constants in
     * the encoder (LR2021_FLRC_SYNC_TX=1 / LR2021_FLRC_SYNC_MATCH=1), so byte1
     * carries (1<<6)|(1<<3)|(pkt_fmt<<2)|crc regardless of any caller value.
     * With crc_width=0xFF -> &0x03 = 0x03, pkt_fmt=1 (mode=1):
     *   out[1] = (1<<6)|(1<<3)|(1<<2)|0x03 = 0x40|0x08|0x04|0x03 = 0x4F. */
    struct radio_flrc_config c = base_fixed();
    c.preamble_len = 0xFF; c.sync_len = 0xFF;
    c.crc_width = 0xFF;
    __u8 out[4] = {0};
    lr2021_flrc_encode_params(&c, out);
    assert(out[0] == ((0x0F << 2) | 0x03));               /* 0x3F */
    assert(out[1] == ((1 << 6) | (1 << 3) | (1 << 2) | 0x03)); /* 0x4F */
}

static void test_fifo_irq_default(void) {
    /* Default config (tx_low_threshold=192). RX-high enable=0x04, TX-low
     * enable=0x02, rx_high_threshold=128 (0x00,0x80), tx_low_threshold=192
     * (0x00,0xC0). These are the bench-validated bytes the two driver sites
     * emit -- must match byte-for-byte. */
    struct radio_flrc_config c = base_fixed();
    __u8 out[6] = {0};
    lr2021_flrc_encode_fifo_irq(&c, out);
    assert(out[0] == 0x04);
    assert(out[1] == 0x02);
    assert(out[2] == 0x00);
    assert(out[3] == 0x80);  /* LR2021_RX_HIGH_THRESHOLD = 128 */
    assert(out[4] == 0x00);
    assert(out[5] == 0xC0);  /* tx_low_threshold = 192 */
}

static void test_fifo_irq_nondefault_tx_low(void) {
    /* Non-default tx_low_threshold exercises both threshold bytes; the RX-high
     * bytes and the enable bytes stay pinned. 300 = 0x012C. */
    struct radio_flrc_config c = base_fixed();
    c.tx_low_threshold = 300;
    __u8 out[6] = {0};
    lr2021_flrc_encode_fifo_irq(&c, out);
    assert(out[0] == 0x04);
    assert(out[1] == 0x02);
    assert(out[2] == 0x00);
    assert(out[3] == 0x80);  /* rx_high_threshold unchanged (128) */
    assert(out[4] == 0x01);  /* tx_low_threshold(15:8) */
    assert(out[5] == 0x2C);  /* tx_low_threshold(7:0)  */
}

int main(void) {
    test_fixed_511();
    test_dynamic_clears_pktfmt();
    test_crc_width_codes();
    test_full_pld_len_range();
    test_field_masking();
    test_fifo_irq_default();
    test_fifo_irq_nondefault_tx_low();
    printf("All flrc cfg tests passed!\n");
    return 0;
}
