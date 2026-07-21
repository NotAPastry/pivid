/* LR2021 hardware reset + bring-up sequence.
 *
 * lr2021_hw_reset(): drive NRESET via GPIO. Pure GPIO, no SPI.
 * lr2021_hw_init():  push the 14-command FLRC bring-up sequence over SPI.
 *
 * All command argument byte layouts are cross-referenced against the
 * LR2021/LR2022/LR2012 V2.1 datasheet (text dump at
 * docs/datasheets/pdftotext/62785538.LR20xxDatasheet_V2_1.txt). Section
 * numbers are quoted at each step.
 *
 * Callers:
 *   - lr2021_probe: once at module load
 *   - lr2021_tx.c worker (hard-recovery path)
 *   - RADIO_IOCTL_RESET ioctl
 *
 * lr2021_hw_init takes dev->cmd_mutex for the whole sequence: a fresh
 * bring-up needs to be atomic w.r.t. ioctls and the IRQ handler. The
 * primitives (lr2021_spi_cmd) require caller-side locking -- see
 * lr2021_spi.h locking contract. */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/types.h>
#include "lr2021.h"
#include "lr2021_commands.h"
#include "lr2021_flrc_cfg.h"
#include "lr2021_rf_cfg.h"
#include "lr2021_spi.h"
#include "lr2021_status.h"   /* lr2021_decode_errors */

/* Placeholder FLRC syncword loaded by hw_init. Userspace overrides it via
 * RADIO_IOCTL_SET_SYNC_WORD with the value the daemon derives from
 * BINDING_PHRASE in /opt/pivid/pivid.conf (see common/phrase_sync.c),
 * so this default is only on-air before the daemon configures the chip. */
#define LR2021_DEFAULT_SYNC_WORD 0x5A5A5A5Au

void lr2021_hw_reset(LR2021_Device *dev) {
    if (!dev->nreset) {
        /* Defensive: probe() acquires the NRESET descriptor before
         * registering anything that calls hw_reset (the RESET ioctl,
         * the TX worker recovery path), so this branch is unreachable
         * in practice. Bail rather than NULL-deref if a future
         * probe-ordering change ever inverts that invariant. */
        return;
    }

    /* The DT overlay declares the NRESET line as GPIO_ACTIVE_LOW,
     * so gpiod_set_value(d, 1) asserts (pulls the physical line low) and
     * gpiod_set_value(d, 0) releases (drives high). Datasheet section 5.3
     * specifies the NRESET pulse must be at least 100 us; 1 ms gives
     * comfortable margin. After release, the chip runs its internal POR /
     * autoboot sequence; 5 ms covers the worst-case startup latency
     * before the first SPI command will be accepted. */
    gpiod_set_value(dev->nreset, 1);
    usleep_range(1000, 2000);
    gpiod_set_value(dev->nreset, 0);
    msleep(5);
}

int lr2021_hw_init(LR2021_Device *dev) {
    /* Step argument tables. All values are statically composed from the
     * datasheet bit layouts; the comments below name the field-to-byte
     * mapping so a future reader can verify against the datasheet
     * without re-deriving. */

    /* Defensive clamp: a recovery / re-init path must never push a garbage
     * pld_len to the chip. If dev->flrc_cfg looks unseeded or out of range,
     * fall back to the fastest proven default (fixed-511, cr 3/4, 16-bit
     * CRC). Done before the param buffers below are built from flrc_cfg. */
    if (dev->flrc_cfg.pld_len < 6 || dev->flrc_cfg.pld_len > 511) {
        dev->flrc_cfg.mode = 1;
        dev->flrc_cfg.cr = 1;
        dev->flrc_cfg.crc_width = 1;
        dev->flrc_cfg.preamble_len = 7;
        dev->flrc_cfg.sync_len = 2;
        dev->flrc_cfg.pld_len = 511;
        dev->flrc_cfg.tx_low_threshold = 192;
    }

    /* 1) SetStandby (6.3.2, table 6-8): 1 byte standby_mode.
     *    0 = RC oscillator (the only mode this driver uses; XOSC is
     *    reserved for time-critical chip-side timing we don't need). */
    static const u8 standby_rc[1] = { LR2021_STANDBY_MODE_RC };

    /* 2) SetRegMode (6.3.18, table 6-26): 1 byte simo_usage.
     *    0x02 = SIMO_NORMAL: DC-DC for VDCC + VPAX during Tx. Required
     *    for the +12 dBm HF PA to hit its rated efficiency. */
    static const u8 reg_mode_simo[1] = { 0x02 };

    /* 3) SetPaConfig (7.4.1, table 7-15): 3 bytes. Field-to-bit split per
     *    the table's stacked-cell convention, top = MSB (proven twice on
     *    this hardware: table 18-4 cr sits in the HIGH nibble -- the CR
     *    3/4->1 airtime delta measured it -- and table 7-2 rx_path=bit 0
     *    is what makes 0x01 select the working HF path).
     *    Byte 0: pa_sel(7) | rfu(6:2) | pa_lf_mode(1:0) -> 0x80
     *            pa_sel=1 (HF PA), pa_lf_mode=0 (LF FSM default; 1-3 RFU).
     *            (The pre-2026-07 value 0x06 had pa_sel=0 + RFU bits set;
     *            it worked only because SelPa below forces HF anyway.)
     *    Byte 1: pa_lf_duty_cycle(7:4) | pa_lf_slices(3:0) -> 0x67
     *            LF PA unused: spec says duty=6, slices=7 exactly.
     *    Byte 2: rfu(7:5) | pa_hf_duty_cycle(4:0) -> 0x10
     *            pa_hf_duty_cycle=16: the table 7-18 value for +12 dBm
     *            (and the spec minimum; 16-31 authorized). */
    static const u8 pa_config[3] = { 0x80, 0x67, 0x10 };

    /* 4) SelPa (7.4.2, table 7-19): 1 byte. pa_sel=1 -> HF PA.
     *    SetPaConfig already selected HF; SelPa makes the runtime switch
     *    explicit per the datasheet "must be called after SetPaConfig"
     *    sequencing note. */
    static const u8 sel_pa_hf[1] = { 0x01 };

    /* 5) SetRfFrequency (7.3.1, table 7-1): 4 bytes rf_freq in Hz, MSB
     *    first (rf_freq(31:24) at byte 0).
     *    Bytes computed from dev->rf_freq_hz by lr2021_encode_rf_freq().
     *    Must stay within the module's HF range (clamped at probe). */
    u8 rf_freq_2030mhz[4];

    /* 6) SetRxPath (7.3.2, table 7-2): 2 bytes.
     *    Byte 0: rfu(6:0) | rx_path  -> 0x01 (HF path)
     *    Byte 1: rfu(4:0) | rx_boost(2:0) -> dev->rx_boost (module param via
     *            pivid.conf RX_BOOST, clamped 0..7 at probe; default 4 =
     *            non-boosted HF reference gain, datasheet dump 2954).
     *            MEASURED 2026-06-05: rx_boost=7
     *            (boosted) gave NO CRC-loss improvement (chip-side CRC stayed
     *            ~11%, identical to boost=4) and coincided with sustained RX
     *            FIFO-overflow flags, so the extra front-end gain buys nothing
     *            here and is not worth the current. Keep the proven 4.
     *            (The AN1200.104 §7.3 example uses 0x00 = LF non-boost.)
     *            2026-07-04 (DEEP_RESULTS IMP-3): a TX-power sweep 12 -> -18 dBm
     *            (the full 30 dB electronic range) only pushed RX to -62 dBm --
     *            still ~37 dB above the FLRC sensitivity floor (~-99 dBm) -- with
     *            fec loss flat at ~0%. This bench CANNOT reach a sensitivity-
     *            limited condition, so boost's front-end gain is un-testable here
     *            (both the old and any new bench A/B are inconclusive by
     *            construction). rx_boost 4-vs-7 is a FLIGHT/range-test question;
     *            keep 4 until a real range test can isolate the sensitivity limit.
     *            2026-07-06 (DS rev 2.1 audit): the boost delta IS spec'd -- table
     *            3-8 LOSS_S_BOOST = 0.5 dB on HF (1 dB LF), and the RX-current cost
     *            is IDDDELTA2 = +0.4 mA (an older "datasheet-unspecified" note here
     *            was stale). So the prize is ~0.5 dB, not several dB. Sensitivity
     *            tables (incl. FLRC table 3-13, -99 dBm) are taken at rx_boost=7,
     *            but blocking/ACR/co-channel rejection are tabulated NON-boosted
     *            (sec 3.4) -- boosted-mode linearity is uncharacterized, which is
     *            the concrete reason not to blind-default 7 in a dirty RF field. */
    u8 rx_path_hf[2] = { 0x01, dev->rx_boost };

    /* 7) CalibFE (6.4.2, table 6-29): issued with NO arguments. Placed here --
     *    AFTER SetRfFrequency (5) and SetRxPath (6) -- so the chip self-calibrates
     *    the Rx front end (Rx + PLL) for the *current* RF frequency and Rx path.
     *    Per DS 6.4.2 the no-arg form picks the calibration slot by truncating the
     *    current RF frequency DOWN to a 4 MHz multiple ("always lower") -- the SAME
     *    convention SetRx (DS 6.3.5) uses to look the slot up on Rx entry, so the
     *    store and the lookup agree by construction at any carrier, with no
     *    host-side slot encode to get wrong.
     *    The configured carrier is normalized to the 4 MHz FE-cal grid upstream
     *    (pivid-config-agent / common/settings.c snap_rf_freq_to_calib_grid), so
     *    the truncate-down slot lands EXACTLY on the carrier -- the front end is
     *    calibrated dead-centre, not up to 2 MHz off. (Replaces the old bring-up
     *    step 2, which sent an explicit round-to-nearest Freq1 via a hand-rolled
     *    encoder; see DEEP_RESULTS.md BUG-1 for the HW measurement that showed the
     *    old form was accepted by the chip but was needlessly indirect.)
     *    No arg array to declare; sent via LR2021_INIT_STEP_NOARG below. */

    /* 8) SetPacketType (8.1.1, table 8-1): 1 byte. FLRC = 0x05. */
    static const u8 packet_type_flrc[1] = { LR2021_PACKET_TYPE_FLRC };

    /* 9) SetFlrcModulationParams (18.4.1, table 18-4): 2 bytes.
     *    Byte 0: bitrate_bw = 0x00 -> 2600 kbps / 2666 kHz BW (the
     *            highest FLRC rate; minimizes air time for 1080p H.264).
     *    Byte 1: cr(3:0) << 4 | pulse_shape(3:0) = (1 << 4) | 7 = 0x17
     *            cr=1 -> 3/4 FEC, pulse_shape=7 -> Gaussian BT 1.0.
     *     Built at runtime from dev->flrc_cfg via the shared pure encoder so
     *     init and RADIO_IOCTL_SET_FLRC_CONFIG emit identical bytes. Yields
     *     { 0x00, 0x17 }; mod_params do not depend on the framing mode. */
    u8 flrc_mod_params[2];
    lr2021_flrc_encode_mod_params(&dev->flrc_cfg, flrc_mod_params);

    /* 10) SetFlrcPacketParams (18.4.2, table 18-5): 4 bytes.
     *     Byte 0: rfu(1:0)<<6 | agc_pbl_len(3:0)<<2 | sync_len(1:0)
     *             = (7 << 2) | 2 = 0x1E
     *             agc_pbl_len=7 -> 32-bit preamble for AGC settle,
     *             sync_len=2 -> 32-bit syncword.
     *     Byte 1: sync_tx(1:0)<<6 | sync_match(2:0)<<3 | pkt_fmt<<2 | crc(1:0)
     *             = (1<<6) | (1<<3) | (1<<2) | 1 = 0x4D
     *             sync_tx=1 -> use Syncword slot 1 on Tx,
     *             sync_match=1 -> MATCH_1 on Rx (matches sync_tx),
     *             pkt_fmt=0 -> dynamic-length payload: this bit is cleared
     *               (the fixed-length value would be 0x4D; dynamic is 0x49)
     *               and the on-air header carries the length. Dynamic is the
     *               only framing the link runs.
     *             crc=1 -> 16-bit CRC.
     *     Byte 2-3: pld_len MSB, LSB -> 0x01, 0xFF -> 511-byte ceiling. In
     *               dynamic mode pld_len is the per-packet upper bound,
     *               reprogrammed per send by the TX worker.
     *
     *     Built from dev->flrc_cfg via the shared pure encoder so init and
     *     RADIO_IOCTL_SET_FLRC_CONFIG produce identical bytes, honouring
     *     dev->flrc_cfg.mode (pinned to dynamic at probe). The FLRC_PROBE build
     *     also forces dynamic locally for the per-packet length probe. */
    u8 flrc_pkt_params[4];
#ifdef FLRC_PROBE
    {
        struct radio_flrc_config probe_cfg = dev->flrc_cfg;
        probe_cfg.mode = 0;            /* dynamic for the probe */
        lr2021_flrc_encode_params(&probe_cfg, flrc_pkt_params);
    }
#else
    lr2021_flrc_encode_params(&dev->flrc_cfg, flrc_pkt_params);
#endif

    /* 11) SetTxParams (7.4.3, table 7-20): 2 bytes.
     *     Byte 0: tx_power as signed int8 in 0.5 dB steps, HF range
     *             [-39:24] -> [-19.5:+12] dBm; byte1 = PA ramp index
     *             (table 7-21, dev->tx_ramp_idx, module param). Bytes
     *             computed by lr2021_encode_tx_params(). */
    u8 tx_params[2];

    /* 12) SetFlrcSyncword (18.4.5, table 18-10): 5 bytes.
     *     Byte 0: sw_num. Valid values are 1, 2, 3 (1-INDEXED per spec).
     *             We use slot 1 because step 10 above set sync_tx=1 and
     *             sync_match=MATCH_1.
     *     Bytes 1-4: Syncword(31:24)...Syncword(7:0) MSB-first.
     *     Syncword is the placeholder default; userspace overrides via the
     *     SET_SYNC_WORD ioctl with the phrase-derived word from pivid.conf. */
    static const u8 flrc_syncword[5] = {
        1,
        (u8)((LR2021_DEFAULT_SYNC_WORD >> 24) & 0xff),
        (u8)((LR2021_DEFAULT_SYNC_WORD >> 16) & 0xff),
        (u8)((LR2021_DEFAULT_SYNC_WORD >>  8) & 0xff),
        (u8)( LR2021_DEFAULT_SYNC_WORD        & 0xff),
    };

    /* 13) SetDioFunction (6.8.1, table 6-44): 2 bytes.
     *     Byte 0: Dio id. Allowed range 5..11; we drive DIO9.
     *     Byte 1: Func(3:0)<<4 | pull_drive(3:0)
     *             = (1 << 4) | 0 = 0x10
     *             Func=1 -> IRQ output, pull_drive=0 -> no pull in sleep
     *             (we never enter Sleep mode anyway). */
    static const u8 dio_function[2] = { LR2021_DIO_IRQ_LINE, 0x10 };

    /* 14) SetDioIrqConfig (6.8.3, table 6-46): 5 bytes.
     *     Byte 0: Dio id (must match SetDioFunction above).
     *     Bytes 1-4: irq_mask(31:24)...irq_mask(7:0) MSB-first.
     *     Mask comes from LR2021_IRQ_MASK_FLRC -- the FLRC-relevant
     *     subset of IRQ bits, datasheet section 5.7. */
    static const u8 dio_irq_config[5] = {
        LR2021_DIO_IRQ_LINE,
        (u8)((LR2021_IRQ_MASK_FLRC >> 24) & 0xff),
        (u8)((LR2021_IRQ_MASK_FLRC >> 16) & 0xff),
        (u8)((LR2021_IRQ_MASK_FLRC >>  8) & 0xff),
        (u8)( LR2021_IRQ_MASK_FLRC        & 0xff),
    };

    int ret;

    /* Compute the two RF-parameter byte arrays from the device fields set
     * at probe time from module params (rf_freq_hz, rf_tx_power_dbm). These
     * are pure arithmetic -- no SPI, no mutex needed. The arrays were declared
     * above (uninitialized u8[]) and are filled here before the INIT_STEPs
     * that consume them (SetRfFrequency, SetTxParams). CalibFE takes no args. */
    lr2021_encode_rf_freq ((__u64)dev->rf_freq_hz, rf_freq_2030mhz);
    lr2021_encode_tx_params(dev->rf_tx_power_dbm, dev->tx_ramp_idx, tx_params);

    /* Hold cmd_mutex across the whole sequence so an ioctl or IRQ can't
     * interleave a stray command between, e.g., SetPacketType(FLRC) and
     * SetFlrcModParams -- the chip would CMD_FAIL the latter if the
     * packet type was switched away mid-sequence. */
    mutex_lock(&dev->cmd_mutex);

#define LR2021_INIT_STEP(label, opcode, argbuf)                          \
    do {                                                                 \
        ret = lr2021_spi_cmd(dev, (opcode),                              \
                             (argbuf), sizeof(argbuf), NULL, 0);         \
        if (ret) {                                                       \
            dev_err(&dev->spi->dev,                                      \
                    "hw_init: " label " failed (%d)\n", ret);            \
            goto out_unlock;                                             \
        }                                                                \
    } while (0)

/* No-argument variant for commands whose behaviour is defined by the chip's
 * current state (e.g. CalibFE with no Freqn args self-selects freq/path). */
#define LR2021_INIT_STEP_NOARG(label, opcode)                            \
    do {                                                                 \
        ret = lr2021_spi_cmd(dev, (opcode), NULL, 0, NULL, 0);           \
        if (ret) {                                                       \
            dev_err(&dev->spi->dev,                                      \
                    "hw_init: " label " failed (%d)\n", ret);            \
            goto out_unlock;                                             \
        }                                                                \
    } while (0)

    LR2021_INIT_STEP("SetStandbyRC",        LR2021_CMD_SET_STANDBY,            standby_rc);
    LR2021_INIT_STEP("SetRegMode(SIMO)",    LR2021_CMD_SET_REG_MODE,           reg_mode_simo);
    LR2021_INIT_STEP("SetPaConfig",         LR2021_CMD_SET_PA_CONFIG,          pa_config);
    LR2021_INIT_STEP("SelPa(HF)",           LR2021_CMD_SET_SEL_PA,             sel_pa_hf);
    LR2021_INIT_STEP("SetRfFrequency",      LR2021_CMD_SET_RF_FREQUENCY,       rf_freq_2030mhz);
    LR2021_INIT_STEP("SetRxPath(HF)",       LR2021_CMD_SET_RX_PATH,            rx_path_hf);
    /* CalibFE no-arg: MUST follow SetRfFrequency + SetRxPath so the chip
     * self-calibrates the current freq/path (see step 7 comment above). */
    LR2021_INIT_STEP_NOARG("CalibFE(no-arg)", LR2021_CMD_CALIB_FE);
    LR2021_INIT_STEP("SetPacketType(FLRC)", LR2021_CMD_SET_PACKET_TYPE,        packet_type_flrc);
    LR2021_INIT_STEP("SetFlrcModParams",    LR2021_CMD_SET_FLRC_MOD_PARAMS,    flrc_mod_params);
    LR2021_INIT_STEP("SetFlrcPacketParams", LR2021_CMD_SET_FLRC_PACKET_PARAMS, flrc_pkt_params);
    LR2021_INIT_STEP("SetTxParams",         LR2021_CMD_SET_TX_PARAMS,          tx_params);
    LR2021_INIT_STEP("SetFlrcSyncword",     LR2021_CMD_SET_FLRC_SYNCWORD,      flrc_syncword);
    LR2021_INIT_STEP("SetDioFunction",      LR2021_CMD_SET_DIO_FUNCTION,       dio_function);
    LR2021_INIT_STEP("SetDioIrqConfig",     LR2021_CMD_SET_DIO_IRQ_CONFIG,     dio_irq_config);

#undef LR2021_INIT_STEP
#undef LR2021_INIT_STEP_NOARG

    /* Clear any IRQ the chip latched during bring-up so DIO9 starts LOW before
     * the threaded IRQ is requested. The wedge this ultimately guards against
     * (chip boots DIO9-high; lr2021_dio9_thread is the only caller of
     * GetAndClearIrq, so nothing clears the line) is PRIMARILY prevented by
     * requesting the IRQ LEVEL_HIGH (see the devm_request_threaded_irq comment
     * in lr2021_core.c) -- a level handler re-dispatches on a still-high line,
     * so it cannot deadlock the way an edge-rising request can. This init clear
     * is belt-and-suspenders: it arms request_threaded_irq() on a clean line
     * and avoids an immediate spurious thread dispatch right after probe.
     * Observed 2026-06-04: a node booted with gpio27/dio9 stuck `in hi`, TX
     * wedged (no TX_DONE) / RX deaf, unrecoverable by NRESET. */
    {
        u8 irq_clear[4];
        ret = lr2021_spi_cmd(dev, LR2021_CMD_GET_AND_CLEAR_IRQ,
                             NULL, 0, irq_clear, sizeof(irq_clear));
        if (ret) {
            dev_err(&dev->spi->dev,
                    "hw_init: GetAndClearIrq failed (%d)\n", ret);
            goto out_unlock;
        }
    }

    /* Enable + threshold the TX-FIFO-low and RX-FIFO-high IRQs (ConfigFifoIrq,
     * datasheet 6.10.1) that drive the >256 B refill/drain.
     *
     * TX side (lr2021_tx_refill_locked): without the TX-low watermark the chip
     * uses its power-on default, which fires the refill too late to keep the
     * 256 B FIFO fed while the chip drains it at the FLRC info rate -- every
     * >256 B packet then TxFifoUnderflows and leaks garbage past the underrun
     * point, corrupting the on-air frame (observed 2026-06-04: tx_fifo_underflow
     * ~= packets_tx at pld_len 511; RX got bad_hdr/CRC-fail).
     *
     * RX side (lr2021_dio9_thread RxFifo-high drain): the 256 B RX FIFO
     * overflows on a >256 B payload (up to 511) unless the host reads it out
     * WHILE receiving (datasheet §5.3.1). RxFifo-high fires at
     * LR2021_RX_HIGH_THRESHOLD so the handler drains the FIFO before it fills.
     *
     * The same bytes are sent by lr2021_apply_flrc_config (the SET_FLRC_CONFIG
     * ioctl), but that ioctl is optional and no daemon issues it -- bring-up
     * MUST program the watermarks itself. The 6-byte arg buffer is built by the
     * shared pure encoder (lr2021_flrc_encode_fifo_irq) so this site and the
     * ioctl can't drift. */
    {
        u8 fifo_irq[6];
        lr2021_flrc_encode_fifo_irq(&dev->flrc_cfg, fifo_irq);
        ret = lr2021_spi_cmd(dev, LR2021_CMD_CONFIG_FIFO_IRQ,
                             fifo_irq, sizeof(fifo_irq), NULL, 0);
        if (ret) {
            dev_err(&dev->spi->dev,
                    "hw_init: ConfigFifoIrq failed (%d)\n", ret);
            goto out_unlock;
        }
    }

    /* Track the boot-time syncword so an ioctl that reads dev->sync_word
     * before any SET_SYNC_WORD call sees a sane value. dev->mode is
     * left untouched -- after init the chip is in Standby RC, which has
     * no RADIO_MODE_* equivalent; userspace must explicitly SET_MODE. */
    dev->sync_word = LR2021_DEFAULT_SYNC_WORD;

    /* Clean error baseline after a successful bring-up. Both the chip's
     * ErrorStat and the driver's chip_error_stat latch stickily: the chip
     * clears only on ClearErrors, and lr2021_irq_chip_diag_locked OR-accumulates
     * GetErrors and never clears it. Without this, a transient bit set during
     * the POR/bring-up window (HW-observed 2026-06-18: ErrorStat=0x4000 right
     * after init) sticks in the reported chip_error_stat forever and masquerades
     * as a live fault. Read the bring-up errors (logged once for diagnostics),
     * clear them on the chip, then zero the accumulator so the heartbeat reports
     * only post-bring-up errors.
     *
     * NOTE (2026-07-02 FE-cal): RXFREQ_NO_FE_CAL_ERR (bit 9, 0x0200) stays CLEAR
     * across Rx entries with this bring-up (HW-measured on pivid-rx via a
     * temporary once/sec GetErrors poll; a skip-CalibFE control read 0x0200 with
     * chip_err=1, proving the detector and that a modprobe NRESET wipes the FE
     * cal). The step-7 no-arg CalibFE calibrates the front end at the carrier,
     * which is 4 MHz-grid-aligned upstream so the cal is dead-centre. An earlier
     * note here claimed the chip "re-asserts bit 9 on every Rx entry" with the
     * old (step-2, round-to-nearest) bring-up; that was UNMEASURED and is refuted
     * -- see DEEP_RESULTS.md BUG-1 (the 0x4000 the driver does see at init is
     * bit 14, a bring-up transient cleared by ClearErrors below, not bit 9).
     * A genuinely low RSSI is a real RF/antenna issue, not an FE-cal flag. */
    {
        u8 e_pre[2] = { 0, 0 }, e_post[2] = { 0, 0 };
        lr2021_spi_cmd(dev, LR2021_CMD_GET_ERRORS,   NULL, 0, e_pre,  sizeof(e_pre));
        lr2021_spi_cmd(dev, LR2021_CMD_CLEAR_ERRORS, NULL, 0, NULL,   0);
        lr2021_spi_cmd(dev, LR2021_CMD_GET_ERRORS,   NULL, 0, e_post, sizeof(e_post));
        if (lr2021_decode_errors(e_pre))
            dev_info(&dev->spi->dev,
                     "hw_init: cleared bring-up errors 0x%04x (live now 0x%04x)\n",
                     lr2021_decode_errors(e_pre), lr2021_decode_errors(e_post));
        WRITE_ONCE(dev->chip_error_stat, 0);
    }

    ret = 0;

out_unlock:
    mutex_unlock(&dev->cmd_mutex);
    return ret;
}
