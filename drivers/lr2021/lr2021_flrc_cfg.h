#ifndef LR2021_FLRC_CFG_H
#define LR2021_FLRC_CFG_H

#include <linux/types.h>   /* __u8 / __u16 / __u32 -- kernel + host (libc) */
#include "radio_ioctl.h"   /* struct radio_flrc_config */

/* Encode SetFlrcPacketParams (datasheet 18.4.2, table 18-5) byte layout from
 * a radio_flrc_config. Mirrors the static init buffer in lr2021_init.c.
 *   out[0]: rfu(1:0)<<6 | agc_pbl_len(3:0)<<2 | sync_len(1:0)
 *   out[1]: sync_tx(1:0)<<6 | sync_match(2:0)<<3 | pkt_fmt<<2 | crc(1:0)
 *   out[2]: pld_len(15:8)
 *   out[3]: pld_len(7:0)
 * sync_tx / sync_match are pinned to the init values (slot 1 / MATCH_1) so a
 * runtime config swap can't desync the syncword slot the SET_SYNC_WORD ioctl
 * writes. pkt_fmt = 1 iff mode==1 (fixed). All field codes are masked to
 * their bit widths so an out-of-range userspace value can't bleed into a
 * neighbouring field. Pure: no SPI, no locks; host-tested in
 * tests/test_flrc_cfg.c. */
#define LR2021_FLRC_SYNC_TX     1u   /* use Syncword slot 1 on Tx       */
#define LR2021_FLRC_SYNC_MATCH  1u   /* MATCH_1 on Rx                   */

static inline void lr2021_flrc_encode_params(const struct radio_flrc_config *cfg,
                                             __u8 out[4]) {
    __u8 pkt_fmt = (cfg->mode == 1) ? 1u : 0u;
    out[0] = (__u8)(((cfg->preamble_len & 0x0Fu) << 2) |
                     (cfg->sync_len & 0x03u));
    out[1] = (__u8)(((LR2021_FLRC_SYNC_TX & 0x03u) << 6) |
                    ((LR2021_FLRC_SYNC_MATCH & 0x07u) << 3) |
                    ((__u32)pkt_fmt << 2) |
                     (cfg->crc_width & 0x03u));
    out[2] = (__u8)(cfg->pld_len >> 8);
    out[3] = (__u8)(cfg->pld_len & 0xFFu);
}

/* Encode SetFlrcModulationParams (datasheet 18.4.1, table 18-4) byte layout.
 *   out[0]: bitrate_bw (kept at the init fastest value 0x00 here -- not
 *           exposed in radio_flrc_config; Phase-3 may add it)
 *   out[1]: cr(3:0)<<4 | pulse_shape(3:0)
 * pulse_shape pinned to 7 (Gaussian BT 1.0) per init. */
#define LR2021_FLRC_BITRATE_BW   0x00u  /* 2.6 Mbps fastest             */
#define LR2021_FLRC_PULSE_SHAPE  0x07u  /* Gaussian BT 1.0              */

static inline void lr2021_flrc_encode_mod_params(const struct radio_flrc_config *cfg,
                                                 __u8 out[2]) {
    out[0] = LR2021_FLRC_BITRATE_BW;
    out[1] = (__u8)(((cfg->cr & 0x0Fu) << 4) | (LR2021_FLRC_PULSE_SHAPE & 0x0Fu));
}

/* Encode the ConfigFifoIrq arg buffer (datasheet 6.10.1, table 6-50). The
 * 6 bytes after the opcode, MSB-first:
 *   out[0]: rx_fifo_irq_enable (flag bits; 0x04 = High -> the >256 B RX drain)
 *   out[1]: tx_fifo_irq_enable (flag bits; 0x02 = Low  -> the >256 B TX refill)
 *   out[2..3]: rx_high_threshold(15:8)/(7:0)  -- hardcoded LR2021_RX_HIGH_THRESHOLD
 *   out[4..5]: tx_low_threshold (15:8)/(7:0)  -- from the runtime config
 * The enable flag bits and the RX-high threshold are pinned here (mirroring the
 * other pure encoders' pinned constants); only tx_low_threshold is taken from
 * the config. Pure: no SPI, no locks; host-tested in tests/test_flrc_cfg.c.
 * The two former hand-built sites (lr2021_apply_flrc_config, lr2021_hw_init)
 * now both call this so they can't drift -- the bytes are bench-validated. */
#define LR2021_FLRC_FIFO_FLAG_LOW   0x02u  /* TxFifo-low  (refill)             */
#define LR2021_FLRC_FIFO_FLAG_HIGH  0x04u  /* RxFifo-high (drain)             */
#define LR2021_RX_HIGH_THRESHOLD    128u   /* RxFifo-high watermark, bytes;
                                            * see lr2021.h for the slack
                                            * reasoning. Pinned here so the
                                            * encoder is self-contained.
                                            * MEASURED 2026-06-05: raising this
                                            * to 192 (per the AN1200.104 §2.4.1
                                            * "~200 B" example) made the RX FIFO
                                            * overflow ~3x/packet (rx_fifo_ovf
                                            * 0 -> ~3200/5s) -- the ~192 B SPI
                                            * drain round-trip exceeds the 64 B
                                            * (~197 us) headroom 192 leaves. 128
                                            * gives 128 B (~394 us) of slack. Do
                                            * NOT raise without re-measuring.
                                            * NOTE (2026-06-15): the "holds
                                            * rx_fifo_ovf at 0" that once read
                                            * here was REGIME-SPECIFIC to the
                                            * <=256 B linktest packets of that
                                            * measurement (RxFifo-high barely
                                            * fires). Under 511 B prod packets the
                                            * in-flight >256 B drain touches the
                                            * overflow mark ~once per 128 B
                                            * segment (~4x/packet) -- BENIGN, zero
                                            * byte loss; the raw counter is now
                                            * gated in lr2021_irq.c so it no
                                            * longer counts those.              */

static inline void lr2021_flrc_encode_fifo_irq(const struct radio_flrc_config *cfg,
                                               __u8 out[6]) {
    out[0] = LR2021_FLRC_FIFO_FLAG_HIGH;                  /* rx enable: High   */
    out[1] = LR2021_FLRC_FIFO_FLAG_LOW;                   /* tx enable: Low    */
    out[2] = (__u8)(LR2021_RX_HIGH_THRESHOLD >> 8);       /* rx_high_thr(15:8) */
    out[3] = (__u8)(LR2021_RX_HIGH_THRESHOLD & 0xFFu);    /* rx_high_thr(7:0)  */
    out[4] = (__u8)(cfg->tx_low_threshold >> 8);          /* tx_low_thr(15:8)  */
    out[5] = (__u8)(cfg->tx_low_threshold & 0xFFu);       /* tx_low_thr(7:0)   */
}

#endif /* LR2021_FLRC_CFG_H */
