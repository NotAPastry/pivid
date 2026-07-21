#ifndef LR2021_COMMANDS_H
#define LR2021_COMMANDS_H

/* LR2021 host command opcodes.
 *
 * Verified against LR2021 V2.1 datasheet section 5.6 "Host Commands Summary"
 * (text dump: docs/datasheets/pdftotext/62785538.LR20xxDatasheet_V2_1.txt
 *  lines ~11560-12700) on 2026-05-25.
 *
 * Each opcode is 16 bits, MSB-first on the SPI wire: byte 0 = opcode >> 8,
 * byte 1 = opcode & 0xff. Followed by command-specific argument bytes.
 *
 * The two FIFO direct opcodes (0x0001/0x0002) use a single SPI frame whose
 * total length determines the payload size; all other commands follow the
 * standard two-frame request/response pattern described in section 5.5.
 */

/* --- Direct FIFO commands (single-frame SPI transactions) -------------- */
#define LR2021_CMD_READ_RX_FIFO            0x0001  /* ReadRadioRxFifo */
#define LR2021_CMD_WRITE_TX_FIFO           0x0002  /* WriteRadioTxFifo */

/* --- System configuration / status (Table 5-3) ------------------------- */
#define LR2021_CMD_GET_STATUS              0x0100
#define LR2021_CMD_GET_VERSION             0x0101
#define LR2021_CMD_GET_ERRORS              0x0110
#define LR2021_CMD_CLEAR_ERRORS            0x0111
#define LR2021_CMD_SET_DIO_FUNCTION        0x0112
#define LR2021_CMD_CLEAR_FIFO_IRQ_FLAGS    0x0114
#define LR2021_CMD_CONFIG_FIFO_IRQ         0x011A  /* ConfigFifoIrq 6.10.1 */
#define LR2021_CMD_GET_FIFO_IRQ_FLAGS      0x011B  /* GetFifoIrqFlags 6.10.2 */

/* GetFifoIrqFlags / ConfigFifoIrq flag bits (datasheet 6.10, per-FIFO byte):
 *   0x01 Empty 0x02 Low 0x04 High 0x08 Full 0x10 Overflow 0x20 Underflow */
#define LR2021_FIFO_FLAG_EMPTY     0x01u
#define LR2021_FIFO_FLAG_LOW       0x02u
#define LR2021_FIFO_FLAG_HIGH      0x04u
#define LR2021_FIFO_FLAG_FULL      0x08u
#define LR2021_FIFO_FLAG_OVERFLOW  0x10u
#define LR2021_FIFO_FLAG_UNDERFLOW 0x20u
#define LR2021_CMD_SET_DIO_IRQ_CONFIG      0x0115
/* The DIO line we wire to the host IRQ. Allowed range 5..11 (datasheet 6.8.1);
 * we drive DIO9. SetDioFunction byte 0 and SetDioIrqConfig byte 0 must both
 * carry this id, and it must match the `dio9-gpios` line in the DT overlay. */
#define LR2021_DIO_IRQ_LINE                9
#define LR2021_CMD_CLEAR_IRQ               0x0116
#define LR2021_CMD_GET_AND_CLEAR_IRQ       0x0117  /* GetAndClearIrqStatus */
#define LR2021_CMD_GET_RX_FIFO_LEVEL       0x011C
#define LR2021_CMD_GET_TX_FIFO_LEVEL       0x011D
#define LR2021_CMD_CLEAR_RX_FIFO           0x011E
#define LR2021_CMD_CLEAR_TX_FIFO           0x011F
#define LR2021_CMD_SET_REG_MODE            0x0121
#define LR2021_CMD_CALIBRATE               0x0122
#define LR2021_CMD_CALIB_FE                0x0123
#define LR2021_CMD_SET_AGC_GAIN_MANUAL     0x021A  /* 1 arg byte GainStep(3:0); 0=AGC on, 1..13 fixed */
#define LR2021_CMD_SET_STANDBY             0x0128  /* arg standby_mode: 0=RC, 1=XOSC */
#define LR2021_CMD_SET_FS                  0x0129

/* SET_STANDBY_RC is SET_STANDBY with the standby_mode argument hard-wired
 * to 0 (RC oscillator). Kept as a named alias because the init sequence
 * always uses RC mode -- the design doc never enables XOSC. */
#define LR2021_CMD_SET_STANDBY_RC          LR2021_CMD_SET_STANDBY
#define LR2021_STANDBY_MODE_RC             0x00
#define LR2021_STANDBY_MODE_XOSC           0x01

/* SetRxTxFallbackMode (6.3.7, table 6-13): the mode the chip enters after a
 * TxDone/RxDone/Timeout. POR default is 0x01 (Standby RC), which powers down
 * the HF crystal + unlocks the PLL between packets -- so the next SetTx pays
 * the full TSHFOSC crystal wake-up + PLL relock (table 3-23) before it can
 * ramp. We set FS (0x03): the synthesiser stays locked on our single fixed
 * channel, so back-to-back TX submits go on-air after just the ramp. RX runs
 * continuous (rx_timeout=0xFFFFFF) so it never falls back mid-stream; this is
 * purely a TX-turnaround win. Arg byte = fallback_mode(3:0). */
#define LR2021_CMD_SET_RX_TX_FALLBACK_MODE 0x0206
#define LR2021_FALLBACK_MODE_STDBY_RC      0x01
#define LR2021_FALLBACK_MODE_STDBY_XOSC    0x02
#define LR2021_FALLBACK_MODE_FS            0x03

/* --- Common radio commands (Table 5-4) --------------------------------- */
#define LR2021_CMD_SET_RF_FREQUENCY        0x0200
#define LR2021_CMD_SET_RX_PATH             0x0201
#define LR2021_CMD_SET_PA_CONFIG           0x0202
#define LR2021_CMD_SET_TX_PARAMS           0x0203
#define LR2021_CMD_SET_PACKET_TYPE         0x0207
#define LR2021_CMD_GET_RSSI_INST           0x020B  /* GetRssiInst 7.3.3: resp 2 B after status -- Rssi(8:1), Rssi(0)|rfu(6:0); dBm = -Rssi/2 */
#define LR2021_CMD_SET_RX                  0x020C
#define LR2021_CMD_SET_TX                  0x020D
#define LR2021_CMD_SET_SEL_PA              0x020F  /* SelPa: select LF or HF PA */
#define LR2021_CMD_GET_RX_PACKET_LENGTH    0x0212  /* GetRxPktLength */

/* --- FLRC-specific commands (Table 5-7) -------------------------------- */
#define LR2021_CMD_SET_FLRC_MOD_PARAMS     0x0248
#define LR2021_CMD_SET_FLRC_PACKET_PARAMS  0x0249
#define LR2021_CMD_GET_FLRC_RX_STATS       0x024A
#define LR2021_CMD_GET_FLRC_PACKET_STATUS  0x024B
#define LR2021_CMD_SET_FLRC_SYNCWORD       0x024C  /* used by bind state machine */

/* The plan/code calls "GET_PACKET_STATUS"; in FLRC mode this routes to
 * GetFlrcPacketStatus per datasheet section 5.6.4.4. */
#define LR2021_CMD_GET_PACKET_STATUS       LR2021_CMD_GET_FLRC_PACKET_STATUS

/* --- IRQ status bits (datasheet section 5.7, table 5-17) --------------- *
 *
 * Returned by GetAndClearIrqStatus as a 32-bit word sent MSB-first on the
 * wire -- irqs(31:24) is the first response byte after Stat (DS table
 * 6-49), so the handler assembles buf[0]<<24 .. buf[3]. Only
 * the bits we actually care about are named here; the rest (LoRa-specific,
 * RTToF ranging, intra-packet hopping, low-battery, etc.) are out of
 * scope for the FLRC video-link use case.
 */
#define LR2021_IRQ_SYNC_FAIL               (1u << 13) /* Rx FSK: syncword match failed (FLRC reuses) */
#define LR2021_IRQ_ERROR                   (1u << 16) /* check GetErrors for details */
#define LR2021_IRQ_CMD_ERROR               (1u << 17)
#define LR2021_IRQ_RX_DONE                 (1u << 18)
#define LR2021_IRQ_TX_DONE                 (1u << 19)
#define LR2021_IRQ_TIMEOUT                 (1u << 21) /* Rx/Tx timeout (chip-level) */
#define LR2021_IRQ_CRC_ERROR               (1u << 22) /* received packet failed CRC */
#define LR2021_IRQ_LEN_ERROR               (1u << 23) /* FSK/FLRC/LoRa length error */

/* FIFO-threshold IRQs (datasheet table 5-17, sheet 1): bit0 = RxFifo, bit1 =
 * TxFifo. Verified against the dump 2026-06-04 (bits 0/1 confirmed). Used for
 * the >256 B FLRC payload refill path (ConfigFifoIrq watermarks). */
#define LR2021_IRQ_RX_FIFO                 (1u << 0)  /* Rx FIFO threshold (low/high) */
#define LR2021_IRQ_TX_FIFO                 (1u << 1)  /* Tx FIFO threshold (low) */

/* Mask of IRQs we enable for FLRC operation -- set via SetDioIrqConfig
 * during hw_init. Bits not in this mask never raise DIO9. */
#define LR2021_IRQ_MASK_FLRC \
    (LR2021_IRQ_TX_DONE | LR2021_IRQ_RX_DONE | LR2021_IRQ_TIMEOUT | \
     LR2021_IRQ_CRC_ERROR | LR2021_IRQ_LEN_ERROR | LR2021_IRQ_SYNC_FAIL | \
     LR2021_IRQ_ERROR | LR2021_IRQ_RX_FIFO | LR2021_IRQ_TX_FIFO)

/* --- Packet type values for SetPacketType (datasheet table 8-1) -------- */
#define LR2021_PACKET_TYPE_LORA            0x00
#define LR2021_PACKET_TYPE_FSK             0x02
#define LR2021_PACKET_TYPE_BLE             0x03
#define LR2021_PACKET_TYPE_FLRC            0x05  /* this driver's mode */
#define LR2021_PACKET_TYPE_BPSK            0x06
#define LR2021_PACKET_TYPE_LR_FHSS         0x07

#endif /* LR2021_COMMANDS_H */
