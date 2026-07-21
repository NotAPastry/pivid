#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>   /* msleep -- RSSI cal-quality probe fresh-packet wait */
#include <linux/gpio/consumer.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/poll.h>    /* __poll_t, EPOLLIN/EPOLLRDNORM, poll_wait */
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/mm.h>      /* remap_pfn_range, vm_area_struct, virt_to_phys */
#include <linux/of.h>      /* of_device_id, of_match_table */
#include <linux/slab.h>    /* kzalloc / kfree */
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include "lr2021.h"
#include "lr2021_commands.h"
#include "lr2021_ring.h"
#include "lr2021_flrc_cfg.h" /* radio_flrc_config + pure param encoders */
#include "radio_ioctl.h"   /* uapi (ioctls + mmap offsets) -- found via
                              ccflags-y := -I$(src)/../../common in Kbuild */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PiVid");
MODULE_DESCRIPTION("LR2021 2.4GHz FLRC transceiver driver");

/* FLRC config module params (boot-time defaults; overridden at runtime by
 * RADIO_IOCTL_SET_FLRC_CONFIG). The FLRC layer always runs DYNAMIC outer
 * framing -- there is no mode knob. Defaults: 511 B payload ceiling + 16-bit
 * CRC + 32-bit preamble/sync + CR 3/4. tx_low_threshold default 192 gives the
 * refill slack. */
static unsigned int flrc_pld_len       = 511;
static unsigned int flrc_cr            = 1;     /* 3/4                    */
static unsigned int flrc_crc_width     = 1;     /* 16-bit (DS 18-5: 0=off 1=16 2=24 3=32) */
static unsigned int flrc_preamble_len  = 7;     /* 32-bit                 */
static unsigned int flrc_sync_len      = 2;     /* 32-bit                 */
static unsigned int flrc_tx_low_thresh = 192;
/* Post-TxDone fallback mode for TX: 1 = FS (synth stays locked between TX
 * packets), 0 = Standby RC (chip default). A/B knob; lr2021_apply_mode has the
 * mechanism + the measurement showing no saturation throughput gain. */
static unsigned int tx_fallback_fs     = 1;
module_param(flrc_pld_len,       uint, 0644);
module_param(flrc_cr,            uint, 0644);
module_param(flrc_crc_width,     uint, 0644);
module_param(flrc_preamble_len,  uint, 0644);
module_param(flrc_sync_len,      uint, 0644);
module_param(flrc_tx_low_thresh, uint, 0644);
module_param(tx_fallback_fs,     uint, 0644);
MODULE_PARM_DESC(tx_fallback_fs,     "TX post-TxDone fallback: 1=FS (fast), 0=Standby RC");
MODULE_PARM_DESC(flrc_pld_len,       "FLRC payload length 6..511");
MODULE_PARM_DESC(flrc_cr,            "FLRC coding-rate field code");
MODULE_PARM_DESC(flrc_crc_width,     "FLRC CRC width code (DS table 18-5: 0=off 1=16 2=24 3=32-bit; both ends must match)");
MODULE_PARM_DESC(flrc_preamble_len,  "FLRC agc_pbl_len field code 0..15");
MODULE_PARM_DESC(flrc_sync_len,      "FLRC sync_len field code 0..3");
MODULE_PARM_DESC(flrc_tx_low_thresh, "TX FIFO-low refill watermark (bytes)");

/* RF carrier frequency and TX power. Default carrier is 2100 MHz (S-band), an
 * exact LR2021 4 MHz FE-cal grid point (2100/4 = 525) so the no-arg CalibFE
 * (lr2021_init.c step 7) calibrates dead-centre.
 *
 * NOTE: 2100 MHz sits BELOW the external SZM-2166Y PA's rated 2.3-2.7 GHz band.
 * Driving that amplifier off-band gives unspecified gain, match and linearity.
 * If the SZM-2166Y is fitted, either move the carrier back into 2.3-2.7 GHz or
 * bench-verify output and spectrum at 2100 MHz before transmitting. The antenna
 * match matters too -- a 2.3 GHz antenna is not matched here. Must stay in sync
 * with the
 * config-agent default (common/settings.c settings_apply_defaults) -- deploy.sh
 * installs a comment-only modprobe.d, so this compiled default is the boot
 * carrier until the agent regenerates modprobe.d from pivid.conf. The chip HF
 * PA max is +12 dBm, but the default drives the SZM-2166Y (~36 dB gain, output
 * P1dB ~35 dBm, rated 33 dBm/2 W, input compresses ~-1 dBm). -3 dBm in -> ~33
 * dBm/2 W out, the amp's rated max ~2 dB below P1dB; do NOT raise it (input
 * overdrive -> splatter and likely damage). Bench-measure actual output first
 * -- the module may add its own input matching. Clamped at probe to HF band. */
static unsigned int rf_freq_hz      = 2100000000u; /* S-band carrier, Hz (on the 4 MHz FE-cal grid) */
static int          rf_tx_power_dbm = -3;          /* drives SZM-2166Y ext PA to ~2 W (rated max) */
module_param(rf_freq_hz,      uint, 0644);
module_param(rf_tx_power_dbm, int,  0644);
MODULE_PARM_DESC(rf_freq_hz,      "RF carrier in Hz (HF band 1.9e9..2.5e9)");
MODULE_PARM_DESC(rf_tx_power_dbm, "TX power in dBm (chip HF PA max +12; default -3 sized for an external SZM-2166Y PA)");

/* PA ramp time index (SetTxParams byte 1, datasheet table 7-21: 0x00=2 us ..
 * 0x12=304 us). Applied at probe only, so an A/B is a module reload away.
 *
 * Default 0x03 (16 us) since the 2026-07-05 IMP-2 bench campaign (was 0x0A =
 * 128 us). The ramp is paid TWICE per packet (up+down), so this is
 * ~-224 us/pkt of saturation period. Constraints that picked 0x03:
 *  - RX drain headroom: ramp time doubles as inter-packet FIFO drain budget.
 *    Full 100k-pkt saturation sweep WITH the IMP-2b fast BUSY-wait: clean at
 *    >=16 us (bad_header exactly 4, rx_walk_drops 389-393 in every arm);
 *    first degradation whiskers at 8 us (bad_header 7, stray fragment).
 *    Without IMP-2b, 16 us collapsed at 28% loss -- the BUSY-wait spin is
 *    what buys this margin; revisit if the SPI wait path regresses.
 *  - Spectrum: ACP measured unchanged (+-1 dB) down to 16 us; NOT verified
 *    below. Do not default under 0x03 without re-running the RSSI sweep.
 * See docs/MEASUREMENTS.md "SPI turnaround / PA ramp". */
static unsigned int tx_ramp_idx = 0x03;
module_param(tx_ramp_idx, uint, 0644);
MODULE_PARM_DESC(tx_ramp_idx, "PA ramp index per table 7-21 (0x00=2us..0x12=304us)");

/* Rx front-end boost (SetRxPath byte 1, DS 7.3.2 table 7-2: rx_boost(2:0)).
 * 4 = non-boosted HF reference gain, the proven default (docs/DECISIONS.md);
 * 7 = max boost, ~+0.5 dB HF sensitivity (DS table 3-8 LOSS_S_BOOST) for
 * +0.4 mA (IDDDELTA2) -- blocking/ACR/co-channel are characterized
 * NON-boosted only, so don't default 7 without the interferer bench A/B.
 * Consumed once by lr2021_hw_init's SetRxPath, so a change is a module
 * reload away (pivid.conf RX_BOOST -> modprobe.d -> TIER_MODRELOAD). */
static unsigned int rx_boost = 4;
module_param(rx_boost, uint, 0644);
MODULE_PARM_DESC(rx_boost, "Rx front-end boost 0..7 (DS 7.3.2; 4=HF non-boosted default, 7=max boost)");

/* RSSI calibration-quality probe (RADIO_IOCTL_GET_CAL_DIFF). The LR2021 POR cal
 * is corrupted by incoming RF during its boot window, leaving RSSI ~30 dB low on
 * ~75% of boots. We measure it by reading RSSI at a forced LOW vs forced HIGH
 * gain step: a good (input-referred) cal reads the same at both, a bad cal
 * diverges ~15 dB. GET_CAL_DIFF returns that |dB| -- pivid-rx thresholds it and
 * reloads the module (the only re-roll, and the only reset that doesn't wedge
 * DIO9) until it passes. Probe gain steps tunable without a rebuild. */
static int cal_probe_step    = 3;   /* forced AGC LOW gain step for the probe   */
static int cal_probe_step_hi = 11;  /* forced AGC HIGH gain step for the probe  */
module_param(cal_probe_step, int, 0644);
module_param(cal_probe_step_hi, int, 0644);
MODULE_PARM_DESC(cal_probe_step, "Forced AGC LOW gain step for the cal probe (1..13)");
MODULE_PARM_DESC(cal_probe_step_hi, "Forced AGC HIGH gain step for the cal probe (1..13)");

static struct class *lr2021_class;
static LR2021_Device *lr2021_dev;
static struct dentry *lr2021_debugfs_dir;

/* ---------- file operations ---------------------------------------------- */

static int lr2021_open(struct inode *inode, struct file *filp) {
    LR2021_Device *dev = container_of(inode->i_cdev, LR2021_Device, cdev);
    /* Exclusive open: a second opener would share the one SPSC ring pair and
     * corrupt the multiplexed framing. */
    if (atomic_cmpxchg(&dev->is_open, 0, 1) != 0)
        return -EBUSY;
    filp->private_data = dev;
    return 0;
}

static int lr2021_release(struct inode *inode, struct file *filp) {
    LR2021_Device *dev = filp->private_data;
    (void)inode;
    atomic_set(&dev->is_open, 0);
    return 0;
}

/* ---- ioctl helpers ---------------------------------------------------- */

/* Configure the chip for one of the four user-visible modes. Holds
 * cmd_mutex throughout (caller must NOT hold it). Returns 0 on success or
 * a negative errno. */
static int lr2021_apply_mode(LR2021_Device *dev, int mode) {
    /* SetRx continuous: rx_timeout=0xFFFFFF (datasheet 6.3.5). */
    static const uint8_t set_rx_continuous[3] = { 0xFF, 0xFF, 0xFF };
    /* SetStandby RC: 1-byte standby_mode argument. */
    const uint8_t standby_rc = LR2021_STANDBY_MODE_RC;
    /* RxTxFallbackMode is mode-specific (datasheet 6.3.7):
     *   TX -> FS: keeps the synthesiser locked between packets so the next
     *             SetTx skips the crystal wake-up + PLL relock the default
     *             Standby-RC fallback forces. NOTE (MEASURED 2026-06-05,
     *             docs/MEASUREMENTS.md "TX fallback mode"): at
     *             saturation this gives NO throughput benefit -- the TX path is
     *             host-per-packet-bound (~357 pkt/s regardless of cr/pld/fallback),
     *             not chip-turnaround-bound. Kept (harmless, tx_fifo_underflow=0;
     *             may still trim latency for bursty traffic) and toggleable via
     *             tx_fallback_fs. RX runs continuous (never falls back
     *             mid-stream), so it stays the chip default RC; set explicitly to
     *             undo any FS left over from a prior TX->RX transition. Set here
     *             (not in hw_init) so each mode gets the fallback that suits it. */
    const uint8_t fallback_fs = LR2021_FALLBACK_MODE_FS;
    const uint8_t fallback_rc = LR2021_FALLBACK_MODE_STDBY_RC;
    int ret;

    mutex_lock(&dev->cmd_mutex);

    /* Every mode below issues SetStandby, which flushes the chip's RX FIFO but
     * NOT the host's mid-RX drain accumulation cursor. If a >256 B reception was
     * in progress, rx_drain_off is left non-zero; the next packet's
     * lr2021_drain_rx_packet would then start its read at a stale offset and
     * misframe. Reset it here under cmd_mutex (the only other writer is the DIO9
     * thread, also serialized against the chip via this mutex). */
    dev->rx_drain_off = 0;

    switch (mode) {
    case RADIO_MODE_TX:
    case RADIO_MODE_BIND_TRANSMIT:
        /* The TX worker drives SetTx per packet; here we just park the
         * chip in Standby RC so it's ready and not in some prior Rx
         * window, and set the post-TxDone fallback (FS for fast back-to-back
         * submits, or RC per the tx_fallback_fs module param). */
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_RX_TX_FALLBACK_MODE,
                             tx_fallback_fs ? &fallback_fs : &fallback_rc,
                             sizeof(fallback_fs), NULL, 0);
        if (ret)
            break;
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_STANDBY,
                             &standby_rc, sizeof(standby_rc), NULL, 0);
        break;

    case RADIO_MODE_RX:
    case RADIO_MODE_BIND_LISTEN:
        /* Park in Standby RC first, then enter continuous Rx. Without
         * the Standby step a SetRx issued while the chip is already in
         * Rx (e.g., pivid-rx restarted: kernel module + chip stayed up,
         * but userspace closed and reopened /dev/radio0) leaves the chip
         * in an unresponsive state -- RSSI reads still work but DIO9
         * stops asserting on packet reception. Observed reproducibly
         * 2026-05-30: every pivid-rx restart wedged the chip, recovered
         * only by rmmod + modprobe (NRESET pulse from hw_reset path).
         * Standby->Rx mirrors the TX path above and gives the chip a
         * clean transition boundary. */
        /* Restore the chip-default RC fallback (undo any FS from a prior
         * TX mode) before entering continuous RX. */
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_RX_TX_FALLBACK_MODE,
                             &fallback_rc, sizeof(fallback_rc), NULL, 0);
        if (ret)
            break;
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_STANDBY,
                             &standby_rc, sizeof(standby_rc), NULL, 0);
        if (ret)
            break;
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_RX,
                             set_rx_continuous, sizeof(set_rx_continuous),
                             NULL, 0);
        break;

    default:
        ret = -EINVAL;
        break;
    }

    if (ret == 0)
        dev->mode = (unsigned int)mode;

    mutex_unlock(&dev->cmd_mutex);
    return ret;
}

/* ---- RSSI calibration-quality probe (RADIO_IOCTL_GET_CAL_DIFF) -------- *
 * The chip's power-on firmware calibration (ADC offset / self-reception
 * cancellation) is corrupted when a strong RF signal is present during its
 * calibration window -- which is the normal case (the TX streams continuously).
 * A corrupted cal shifts the reported RSSI baseline ~16 dB AND fools the AGC
 * into parking at a high gain step (another ~16 dB), for a ~30 dB bimodal RSSI
 * error. Reception is unaffected (the link is strong); only the number is wrong.
 * No SPI recalibration re-rolls it -- only an NRESET (POR) does, and the only
 * NRESET that doesn't wedge DIO9 is a full module reload, so the retry lives in
 * pivid-rx (close -> modprobe reload -> reopen). Here we only MEASURE the cal
 * quality (no reset). The helpers below read RSSI at a forced gain with fresh-
 * packet gating; the detector (lr2021_cal_diff) is reference-free. Verified on
 * HW 2026-06-29: 10/10 reload trials and a reboot converged to a correct RSSI. */
static int lr2021_read_rssi_avg(LR2021_Device *dev) {
    u8 pkt[5] = {0};
    int v = 0;
    mutex_lock(&dev->cmd_mutex);
    if (lr2021_spi_cmd(dev, LR2021_CMD_GET_PACKET_STATUS, NULL, 0,
                       pkt, sizeof(pkt)) == 0)
        v = -(int)pkt[2];
    mutex_unlock(&dev->cmd_mutex);
    return v;
}
static void lr2021_set_agc_gain(LR2021_Device *dev, u8 step) {
    u8 g = step & 0x0F;
    mutex_lock(&dev->cmd_mutex);
    lr2021_spi_cmd(dev, LR2021_CMD_SET_AGC_GAIN_MANUAL, &g, 1, NULL, 0);
    mutex_unlock(&dev->cmd_mutex);
}
static u16 lr2021_chip_pktrx(LR2021_Device *dev) {
    u8 buf[6] = {0};
    struct lr2021_flrc_rx_stats s = {0};
    mutex_lock(&dev->cmd_mutex);
    if (lr2021_spi_cmd(dev, LR2021_CMD_GET_FLRC_RX_STATS, NULL, 0,
                       buf, sizeof(buf)) == 0)
        lr2021_decode_flrc_rx_stats(buf, &s);
    mutex_unlock(&dev->cmd_mutex);
    return s.pkt_rx;
}
/* Block until >= `n` new packets are received (chip pkt_rx advanced), so the
 * RSSI we read next reflects the CURRENT gain rather than a stale prior value.
 * Returns false on timeout (no/insufficient signal). */
static bool lr2021_wait_fresh_pkts(LR2021_Device *dev, u16 base, int n, int ms_max) {
    int waited = 0;
    while (waited < ms_max) {
        if ((u16)(lr2021_chip_pktrx(dev) - base) >= (u16)n)
            return true;
        msleep(20);
        waited += 20;
    }
    return false;
}
/* RSSI at a forced LOW gain step vs a forced HIGH gain step, |dB|. RSSI is
 * input-referred, so a GOOD cal reads the same at both (~0-4 dB); a BAD cal's
 * gain compensation breaks down at high gain, so they diverge ~15-17 dB. Using
 * two FORCED steps (not AGC-auto) removes any dependence on the AGC having
 * settled/parked -- the timing-sensitive part that let bad cals slip through.
 * Each read is gated on >=20 fresh packets at that gain (no stale reads). If the
 * link is too quiet to get fresh packets, returns 0 = "good" (no RF during cal
 * => the cal is good). Leaves AGC in auto. */
static int lr2021_read_rssi_at_gain(LR2021_Device *dev, int step, int *out) {
    u16 base;
    lr2021_set_agc_gain(dev, (u8)step);
    base = lr2021_chip_pktrx(dev);
    if (!lr2021_wait_fresh_pkts(dev, base, 20, 1500))
        return -1;                                /* no fresh packets */
    *out = lr2021_read_rssi_avg(dev);
    return 0;
}
static int lr2021_cal_diff(LR2021_Device *dev) {
    int lo = 0, hi = 0, d;
    if (lr2021_read_rssi_at_gain(dev, cal_probe_step, &lo) != 0 ||
        lr2021_read_rssi_at_gain(dev, cal_probe_step_hi, &hi) != 0) {
        lr2021_set_agc_gain(dev, 0);
        return 0;                                 /* no signal -> treat as good */
    }
    lr2021_set_agc_gain(dev, 0);                  /* restore auto */
    d = lo - hi;
    return d < 0 ? -d : d;
}

static int lr2021_apply_sync_word(LR2021_Device *dev, uint32_t sync_word) {
    /* SetFlrcSyncword args (datasheet section 18.4.5, table 18-10):
     *   byte 0   = sw_num. Valid values are 1, 2, 3 -- 1-INDEXED.
     *              sw_num=0 fails on the chip per the spec.
     *   bytes 1-4 = Syncword(31:24) ... Syncword(7:0) MSB-first.
     *
     * We use slot 1 to match lr2021_hw_init: SetFlrcPacketParams was
     * configured with sync_tx=1 / sync_match=MATCH_1 (slot 1), so a
     * runtime swap of the sync word must update the same slot or the
     * chip will keep transmitting / matching the previous value. */
    uint8_t args[5];
    int ret;

    args[0] = 1;
    args[1] = (uint8_t)((sync_word >> 24) & 0xff);
    args[2] = (uint8_t)((sync_word >> 16) & 0xff);
    args[3] = (uint8_t)((sync_word >>  8) & 0xff);
    args[4] = (uint8_t)( sync_word        & 0xff);

    mutex_lock(&dev->cmd_mutex);
    ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_FLRC_SYNCWORD,
                         args, sizeof(args), NULL, 0);
    if (ret == 0)
        dev->sync_word = sync_word;
    mutex_unlock(&dev->cmd_mutex);
    return ret;
}

/* Validate + apply dev->flrc_cfg to the chip. Re-pushes SetFlrcModulationParams,
 * SetFlrcPacketParams, and ConfigFifoIrq (TX FIFO-low watermark). Takes
 * cmd_mutex; callers must NOT hold it. */
int lr2021_apply_flrc_config(LR2021_Device *dev) {
    __u8 mod_params[2];
    __u8 pkt_params[4];
    __u8 fifo_irq[6];
    int  ret;

    /* Range checks: reject obviously-bad config. pld_len bounds are the FLRC
     * dynamic 9-bit range (6..511). */
    if (dev->flrc_cfg.mode > 1)
        return -EINVAL;
    if (dev->flrc_cfg.pld_len < 6 || dev->flrc_cfg.pld_len > 511)
        return -EINVAL;
    if (dev->flrc_cfg.crc_width > 3 || dev->flrc_cfg.sync_len > 3 ||
        dev->flrc_cfg.preamble_len > 15)
        return -EINVAL;

    /* Max on-air length per mode. The silicon's LenError filter discards on-air
     * length headers ABOVE pld_len before the host drains them:
     *   FIXED   : frames are exactly 511                         -> 511
     *   DYNAMIC : largest legit packet is the count=2 FEC pair,
     *             outer FIXED hdr(3) + pair hdr(4) + 2*FEC_SYMBOL_LEN(252) = 511
     * With the 252-byte RS symbol the dynamic max equals the absolute 9-bit
     * ceiling (511), so there is nothing legitimate to tighten below -- leave
     * the default 511. (Pre-252 symbols this tightened to 503.) An explicit
     * smaller pld_len from the ioctl is still honored as-is. */

    lr2021_flrc_encode_mod_params(&dev->flrc_cfg, mod_params);
    lr2021_flrc_encode_params(&dev->flrc_cfg, pkt_params);

    /* ConfigFifoIrq (datasheet 6.10.1, table 6-50). RxFifo-high drives the
     * >256 B drain-during-reception (lr2021_dio9_thread); TxFifo-low drives the
     * >256 B refill (lr2021_tx_refill_locked). The 6-byte layout is built by the
     * shared pure encoder so this site and lr2021_hw_init can't drift. */
    lr2021_flrc_encode_fifo_irq(&dev->flrc_cfg, fifo_irq);

    mutex_lock(&dev->cmd_mutex);
    ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_FLRC_MOD_PARAMS,
                         mod_params, sizeof(mod_params), NULL, 0);
    if (ret == 0)
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_FLRC_PACKET_PARAMS,
                             pkt_params, sizeof(pkt_params), NULL, 0);
    if (ret == 0)
        ret = lr2021_spi_cmd(dev, LR2021_CMD_CONFIG_FIFO_IRQ,
                             fifo_irq, sizeof(fifo_irq), NULL, 0);
    /* We just reprogrammed SetFlrcPacketParams (to dev->flrc_cfg.pld_len), so the
     * dynamic-mode per-packet length cache is now stale -- force the next TX to
     * reprogram. Held under cmd_mutex, the cache's single owner. */
    dev->tx_len_valid = false;
    mutex_unlock(&dev->cmd_mutex);
    return ret;
}

/* GET_RSSI: read FLRC packet status, return rssi_avg expressed as int8_t
 * dBm (signal power = -rssi_avg/2; we report the 1-dBm-LSB top 8 bits
 * directly negated). Bottom 0.5 dB bit is discarded -- ioctl returns
 * a whole-dBm value to keep the uapi int8_t. */
static int lr2021_read_rssi(LR2021_Device *dev, int8_t *out) {
    uint8_t resp[5];
    int     ret;
    int     rssi;

    mutex_lock(&dev->cmd_mutex);
    ret = lr2021_spi_cmd(dev, LR2021_CMD_GET_PACKET_STATUS,
                         NULL, 0, resp, sizeof(resp));
    mutex_unlock(&dev->cmd_mutex);
    if (ret)
        return ret;

    /* resp byte layout (datasheet 18-9):
     *   [0] pkt_len(15:8)
     *   [1] pkt_len(7:0)
     *   [2] rssi_avg(8:1)   <-- 8 MSBs of a 9-bit value, ~1 dBm LSB
     *   [3] rssi_sync(8:1)
     *   [4] rssi_avg(0) at bit 7, rfu, rssi_sync(0) at bit 0
     *
     * Practical FLRC RSSI range is -30 to -130 dBm, fits int8_t fine.
     * If we're ever weaker than -127 dBm, saturate. */
    rssi = -(int)resp[2];
    if (rssi < -128) rssi = -128;
    *out = (int8_t)rssi;
    return 0;
}

/* Snapshot all driver counters into a radio_stats. The uint64 group is read
 * under `lock` for a coherent set; atomics are read lockless. rx_silent_ms is
 * computed here (ms since the last DIO9 IRQ); 0xFFFFFFFF = none yet. */
static void lr2021_fill_stats(LR2021_Device *dev, struct radio_stats *s) {
    unsigned long flags;
    int wr;
    ktime_t last;
    s64 silent_ms;

    memset(s, 0, sizeof(*s));
    s->version = RADIO_STATS_VERSION;
    s->size    = sizeof(*s);

    spin_lock_irqsave(&dev->lock, flags);
    s->packets_tx        = dev->packets_tx;
    s->packets_rx        = dev->packets_rx;
    s->bytes_tx          = dev->bytes_tx;
    s->bytes_rx          = dev->bytes_rx;
    s->errors_spi        = dev->errors_spi;
    s->errors_irq        = dev->errors_irq;
    s->rx_ring_full_drops = dev->rx_ring_full_drops;
    spin_unlock_irqrestore(&dev->lock, flags);

    s->crc       = (u32)atomic_read(&dev->crc_count);
    s->sync      = (u32)atomic_read(&dev->sync_count);
    s->len       = (u32)atomic_read(&dev->len_count);
    s->tmout     = (u32)atomic_read(&dev->tmout_count);
    s->crc_drops = (u32)atomic_read(&dev->crc_drops);
    s->rx_fifo_overflow = (u32)atomic_read(&dev->rx_fifo_overflow_count);
    s->tx_fifo_underflow = (u32)atomic_read(&dev->tx_fifo_underflow_count);

    wr = atomic_read(&dev->wedge_recoveries);
    s->wedge_soft = (u32)(wr & 0xFFFF);
    s->wedge_hard = (u32)((wr >> 16) & 0xFFFF);

    /* deprecated: TX wedge is wedge.c-driven (B8). UAPI field kept for ABI
     * stability; stays memset-zeroed (no strike counter in the IRQ-driven
     * state machine). */

    last = READ_ONCE(dev->last_dio9_ktime);
    if (ktime_to_ns(last) == 0) {
        s->rx_silent_ms = 0xFFFFFFFFu;        /* never received */
    } else {
        silent_ms = ktime_ms_delta(ktime_get(), last);
        if (silent_ms < 0) silent_ms = 0;
        if (silent_ms > 0xFFFFFFFEll) silent_ms = 0xFFFFFFFEll;
        s->rx_silent_ms = (u32)silent_ms;
    }
}

/* debugfs: /sys/kernel/debug/lr2021/stats -- a read-on-demand dump of all
 * driver counters, readable even when userspace is wedged. Non-ABI. */
static int lr2021_stats_show(struct seq_file *m, void *v) {
    struct radio_stats s;
    (void)v;
    if (!lr2021_dev) {
        seq_puts(m, "no device\n");
        return 0;
    }
    lr2021_fill_stats(lr2021_dev, &s);
    seq_printf(m,
        "packets_tx %llu\npackets_rx %llu\nbytes_tx %llu\nbytes_rx %llu\n"
        "errors_spi %llu\nerrors_irq %llu\nrx_ring_full_drops %llu\n"
        "crc %u\nsync %u\nlen %u\ntmout %u\ncrc_drops %u\n"
        "rx_fifo_overflow %u\ntx_fifo_underflow %u\n"
        "wedge_soft %u\nwedge_hard %u\ntx_timeouts_consecutive %u\n"
        "rx_silent_ms %u\n",
        s.packets_tx, s.packets_rx, s.bytes_tx, s.bytes_rx,
        s.errors_spi, s.errors_irq, s.rx_ring_full_drops,
        s.crc, s.sync, s.len, s.tmout, s.crc_drops,
        s.rx_fifo_overflow, s.tx_fifo_underflow,
        s.wedge_soft, s.wedge_hard, s.tx_timeouts_consecutive,
        s.rx_silent_ms);
    /* Chip-side counters (not in the versioned radio_stats UAPI; read straight
     * from the device). cmd_status_err = commands the chip's Stat rejected;
     * chip_error_stat = OR of GetErrors bits; flrc_chip_* = the silicon's own
     * FLRC RX stats (ground truth vs the host crc/len above); chip_mode = the
     * chip's last self-reported mode (1=STDBY_RC 3=FS 4=Rx 5=Tx). */
    seq_printf(m,
        "cmd_status_err %u\nchip_err %u\ncmd_err %u\nchip_error_stat 0x%04x\n"
        "flrc_chip_pkt_rx %u\nflrc_chip_crc_err %u\nflrc_chip_len_err %u\n"
        "chip_mode %u\n",
        (u32)atomic_read(&lr2021_dev->cmd_status_err),
        (u32)atomic_read(&lr2021_dev->chip_err_count),
        (u32)atomic_read(&lr2021_dev->cmd_err_count),
        READ_ONCE(lr2021_dev->chip_error_stat),
        READ_ONCE(lr2021_dev->flrc_rxs_pkt),
        READ_ONCE(lr2021_dev->flrc_rxs_crc),
        READ_ONCE(lr2021_dev->flrc_rxs_len),
        READ_ONCE(lr2021_dev->last_chip_mode));
    /* Wedge chip-liveness gate (non-ABI): RF-quiet episodes where the chip-counter
     * gate skipped a soft recover the legacy watchdog would have fired. */
    seq_printf(m, "wedge_quiet_skips %u\n",
               (u32)atomic_read(&lr2021_dev->wedge_quiet_skips));
    /* Frame-offset decision diagnostics (non-ABI). RF-independent stray probe:
     * rx_frame_stray/rx_frame_total = the 16 MHz SPI stray-byte rate at source;
     * rx_frame_ambig = ambiguous decisions (residual-misframe risk surface). */
    seq_printf(m,
        "rx_frame_total %u\nrx_frame_stray %u\nrx_frame_ambig %u\n",
        (u32)atomic_read(&lr2021_dev->rx_frame_total),
        (u32)atomic_read(&lr2021_dev->rx_frame_stray),
        (u32)atomic_read(&lr2021_dev->rx_frame_ambig));
    /* Multi-packet walker (non-ABI): extra = coalesced packets recovered per
     * the in-band walk; drops = bytes abandoned on an unframable stream head. */
    seq_printf(m,
        "rx_walk_extra %u\nrx_walk_drops %u\n",
        (u32)atomic_read(&lr2021_dev->rx_walk_extra),
        (u32)atomic_read(&lr2021_dev->rx_walk_drops));
    /* IMP-8 (non-ABI): TX records dropped for pld_len < FLRC 6 B minimum.
     * Expected 0 in production (userspace pads the trailer to 6 B). */
    seq_printf(m, "tx_runt_drops %llu\n",
               (unsigned long long)READ_ONCE(lr2021_dev->tx_runt_drops));
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(lr2021_stats);

/* debugfs: /sys/kernel/debug/lr2021/rssi_inst -- one GetRssiInst (7.3.3) read
 * per open+read. With no packet on the air this is the in-band RF noise floor
 * at the tuned carrier, which makes an off-channel-tuned RX a poor-man's
 * adjacent-channel power meter (IMP-2a PA-ramp A/B). Value is the 9-bit Rssi
 * in half-dB units, printed as signed half-dBm (dBm = value / 2). Off the hot
 * path: only a manual read issues SPI. Non-ABI. */
static int lr2021_rssi_inst_show(struct seq_file *m, void *v) {
    u8  resp[2] = {0};
    int ret;
    u16 raw9;
    (void)v;
    if (!lr2021_dev) {
        seq_puts(m, "no device\n");
        return 0;
    }
    mutex_lock(&lr2021_dev->cmd_mutex);
    ret = lr2021_spi_cmd(lr2021_dev, LR2021_CMD_GET_RSSI_INST,
                         NULL, 0, resp, sizeof(resp));
    mutex_unlock(&lr2021_dev->cmd_mutex);
    if (ret) {
        seq_printf(m, "err %d\n", ret);
        return 0;
    }
    /* Response (table 7-4): byte2 = Rssi(8:1), byte3 bit7 = Rssi(0). */
    raw9 = ((u16)resp[0] << 1) | (u16)(resp[1] >> 7);
    seq_printf(m, "rssi_half_dbm -%u\n", (unsigned int)raw9);
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(lr2021_rssi_inst);


static long lr2021_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    LR2021_Device *dev = filp->private_data;

    switch (cmd) {
    case RADIO_IOCTL_SET_MODE: {
        __s32 mode;
        if (copy_from_user(&mode, (__s32 __user *)arg, sizeof(mode)))
            return -EFAULT;
        if (mode < RADIO_MODE_TX || mode > RADIO_MODE_BIND_LISTEN)
            return -EINVAL;

        /* On transition away from TX, drain the worker before we change
         * the chip's mode. Otherwise the worker could write a packet
         * into the TX FIFO and SetTx the chip just after we put it in
         * Rx -- chip would refuse SetTx-while-in-Rx and userspace would
         * see silent packet loss. cancel_work_sync waits for any
         * in-flight invocation to finish; subsequent ioctl-driven
         * queue_work calls re-arm it cleanly. */
        if ((dev->mode == RADIO_MODE_TX ||
             dev->mode == RADIO_MODE_BIND_TRANSMIT) &&
            (mode != RADIO_MODE_TX &&
             mode != RADIO_MODE_BIND_TRANSMIT)) {
            cancel_work_sync(&dev->tx_work);
            { unsigned long f; spin_lock_irqsave(&dev->tx_lock, f);
              dev->tx_inflight = false;
              WRITE_ONCE(dev->last_submit_ktime, 0);
              spin_unlock_irqrestore(&dev->tx_lock, f); }
        }

        return lr2021_apply_mode(dev, mode);
    }

    case RADIO_IOCTL_SET_SYNC_WORD: {
        __u32 sync_word;
        if (copy_from_user(&sync_word, (__u32 __user *)arg, sizeof(sync_word)))
            return -EFAULT;
        return lr2021_apply_sync_word(dev, sync_word);
    }

    case RADIO_IOCTL_TX_ENQUEUE:
        /* Doorbell: userspace already wrote the H.264 payload to the
         * mmap'd tx_ring; just wake the worker. queue_work is idempotent
         * if the worker is already running or pending. */
        if (!dev->tx_wq)
            return -ENODEV;
        queue_work(dev->tx_wq, &dev->tx_work);
        return 0;

    case RADIO_IOCTL_GET_RSSI: {
        __s8 rssi;
        int  ret = lr2021_read_rssi(dev, &rssi);
        if (ret)
            return ret;
        if (copy_to_user((__s8 __user *)arg, &rssi, sizeof(rssi)))
            return -EFAULT;
        return 0;
    }

    case RADIO_IOCTL_GET_CAL_DIFF: {
        __s32 diff = (__s32)lr2021_cal_diff(dev);   /* measure only, no reset */
        if (copy_to_user((__s32 __user *)arg, &diff, sizeof(diff)))
            return -EFAULT;
        return 0;
    }

    case RADIO_IOCTL_RESET:
        /* Hard reset = NRESET pulse + full hw_init re-push. Two races to
         * avoid:
         *   1) TX worker mid-packet (cmd_mutex held during fifo_write+SetTx).
         *      cancel_work_sync drains the worker; we then clear tx_inflight
         *      so a stale "chain in flight" claim can't suppress the next
         *      prime. Wedge recovery is now wedge.c-driven (B8), not a
         *      strike-count carried across runs.
         *   2) Threaded IRQ handler mid-SPI sequence (GetAndClearIrq /
         *      GetRxPktLength / fifo_read all under cmd_mutex). Take
         *      cmd_mutex across the NRESET pulse so the IRQ thread can't
         *      observe the chip mid-reset; release before hw_init so it can
         *      re-acquire (hw_init manages its own locking).
         * After this returns, the chip is in Standby RC -- userspace must
         * issue SET_MODE before transmissions resume. */
        cancel_work_sync(&dev->tx_work);
        { unsigned long f; spin_lock_irqsave(&dev->tx_lock, f);
          dev->tx_inflight = false;
          WRITE_ONCE(dev->last_submit_ktime, 0);
          spin_unlock_irqrestore(&dev->tx_lock, f); }
        mutex_lock(&dev->cmd_mutex);
        lr2021_hw_reset(dev);
        mutex_unlock(&dev->cmd_mutex);
        return lr2021_hw_init(dev);

    case RADIO_IOCTL_GET_STATS: {
        struct radio_stats st;
        lr2021_fill_stats(dev, &st);
        if (copy_to_user((struct radio_stats __user *)arg, &st, sizeof(st)))
            return -EFAULT;
        return 0;
    }

    case RADIO_IOCTL_SET_FLRC_CONFIG: {
        struct radio_flrc_config cfg;
        if (copy_from_user(&cfg, (struct radio_flrc_config __user *)arg,
                           sizeof(cfg)))
            return -EFAULT;
        /* Quiesce TX before re-configuring: a worker mid-submit could write
         * the FIFO under the old pld_len just as we change it. cancel_work_sync
         * drains it; userspace re-kicks via TX_ENQUEUE afterwards. */
        cancel_work_sync(&dev->tx_work);
        dev->flrc_cfg = cfg;
        return lr2021_apply_flrc_config(dev);
    }

#ifdef FLRC_PROBE
    case RADIO_IOCTL_PROBE_DYN_TX: {
        struct radio_probe_dyn_tx a;
        if (copy_from_user(&a, (struct radio_probe_dyn_tx __user *)arg, sizeof(a)))
            return -EFAULT;
        return lr2021_probe_dyn_tx(dev, a.len);
    }
#endif

    default:
        return -ENOTTY;
    }
}

static __poll_t lr2021_poll(struct file *filp, struct poll_table_struct *wait) {
    LR2021_Device *dev = filp->private_data;
    __poll_t mask = 0;

    poll_wait(filp, &dev->rx_waitq, wait);

    /* Readable iff the RX ring has at least one byte of data queued.
     * smp_load_acquire pairs with the smp_store_release the kernel-side
     * push uses, so we see a coherent (wr_idx, payload bytes) view. */
    if (lr2021_ring_used(smp_load_acquire(&dev->rx_ring->wr_idx),
                         READ_ONCE(dev->rx_ring->rd_idx)) > 0) {
        mask |= EPOLLIN | EPOLLRDNORM;
    }
    return mask;
}

static int lr2021_mmap(struct file *filp, struct vm_area_struct *vma) {
    LR2021_Device *dev = filp->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;
    void *target_ring;

    /* Userspace selects which ring to map via the `offset` argument to mmap().
     * The kernel sees that as vm_pgoff = offset / PAGE_SIZE. Offsets are
     * defined in common/radio_ioctl.h:
     *   RADIO_MMAP_TX_OFFSET = 0                  -> TX ring (RW from userspace)
     *   RADIO_MMAP_RX_OFFSET = RADIO_RING_BYTES   -> RX ring (RO from userspace)
     *
     * The size of each ring is RADIO_RING_BYTES (header page + power-of-2
     * data area). */
    if (size != RADIO_RING_BYTES) {
        return -EINVAL;
    }

    if (vma->vm_pgoff == 0) {
        target_ring = dev->tx_ring;
    } else if (vma->vm_pgoff == (RADIO_RING_BYTES >> PAGE_SHIFT)) {
        /* RX ring: kernel writes wr_idx + data bytes; userspace writes rd_idx
         * (consumer-advances-rd_idx in the SPSC contract). Userspace needs
         * write access so the rd_idx atomic store lands -- the data bytes are
         * read-only from userspace by convention, not by enforcement. The
         * _pad0/_pad1 cachelines in struct lr2021_ring prevent false-sharing
         * between the kernel's wr_idx writes and userspace's rd_idx writes. */
        target_ring = dev->rx_ring;
    } else {
        return -EINVAL;
    }

    /* VM_DONTEXPAND: mremap() must not extend the mapping past one ring's
     *   worth of bytes -- the ioctl/poll/mmap contract depends on size ==
     *   RADIO_RING_BYTES per mapping.
     * VM_DONTDUMP:   keep ring contents (and the indices, which can be in
     *   flight) out of coredumps; large + churning + uninteresting. */
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

    /* remap_pfn_range with virt_to_phys() assumes the ring was allocated from
     * the kernel direct-map (kmalloc / __get_free_pages). If lr2021_ring_init()
     * ever switches to vmalloc(), this must change to remap_vmalloc_range(). */
    if (remap_pfn_range(vma, vma->vm_start,
                        virt_to_phys(target_ring) >> PAGE_SHIFT,
                        size, vma->vm_page_prot)) {
        return -EAGAIN;
    }

    return 0;
}

static const struct file_operations lr2021_fops = {
    .owner          = THIS_MODULE,
    .open           = lr2021_open,
    .release        = lr2021_release,
    .unlocked_ioctl = lr2021_ioctl,
    .mmap           = lr2021_mmap,
    .poll           = lr2021_poll,
};

/* ---------- probe / remove ---------------------------------------------- */

static int lr2021_probe(struct spi_device *spi) {
    int ret;

    lr2021_dev = kzalloc(sizeof(*lr2021_dev), GFP_KERNEL);
    if (!lr2021_dev)
        return -ENOMEM;

    lr2021_dev->spi = spi;
    spin_lock_init(&lr2021_dev->lock);
    spin_lock_init(&lr2021_dev->tx_lock);
    mutex_init(&lr2021_dev->cmd_mutex);

    /* Mid-TX FIFO refill buffer (>256B packet path). kmalloc'd so it is in the
     * kernel direct-map and DMA-safe for the bcm2835 SPI >96B DMA path. Sized
     * LR2021_RING_MAX_RECORD (513 >= the 511 FLRC max payload). Allocated here,
     * before any goto-guarded resource, so its only failure path is the bare
     * kfree(lr2021_dev) below. */
    lr2021_dev->tx_refill_buf = kmalloc(LR2021_RING_MAX_RECORD, GFP_KERNEL);
    if (!lr2021_dev->tx_refill_buf) {
        kfree(lr2021_dev);
        return -ENOMEM;
    }

    /* DMA-safe SPI scratch buffers: replace the per-call kzalloc in the SPI
     * leaf ops (lr2021_spi.c). Same direct-map/DMA-safe rationale as
     * tx_refill_buf; sized LR2021_SPI_SCRATCH_* (opcode + one 256 B FIFO chunk).
     * Owned by cmd_mutex. Allocated before any goto-guarded resource, so a
     * failure here unwinds with the bare kfree chain below. */
    lr2021_dev->spi_tx_buf = kmalloc(LR2021_SPI_SCRATCH_TX, GFP_KERNEL);
    lr2021_dev->spi_rx_buf = kmalloc(LR2021_SPI_SCRATCH_RX, GFP_KERNEL);
    if (!lr2021_dev->spi_tx_buf || !lr2021_dev->spi_rx_buf) {
        kfree(lr2021_dev->spi_rx_buf);
        kfree(lr2021_dev->spi_tx_buf);
        kfree(lr2021_dev->tx_refill_buf);
        kfree(lr2021_dev);
        return -ENOMEM;
    }

    /* Seed FLRC runtime config from module params (fastest default). hw_init
     * pushes equivalent static buffers; this struct is consumed by
     * RADIO_IOCTL_SET_FLRC_CONFIG / lr2021_apply_flrc_config. The framing mode
     * is fixed at DYNAMIC -- the only mode the link runs -- and is not a knob. */
    lr2021_dev->flrc_cfg.mode             = 0;   /* DYNAMIC: the only framing */
    lr2021_dev->flrc_cfg.cr               = (__u8)flrc_cr;
    lr2021_dev->flrc_cfg.crc_width        = (__u8)flrc_crc_width;
    lr2021_dev->flrc_cfg.preamble_len     = (__u8)flrc_preamble_len;
    lr2021_dev->flrc_cfg.sync_len         = (__u8)flrc_sync_len;
    lr2021_dev->flrc_cfg.pld_len          = (__u16)flrc_pld_len;
    lr2021_dev->flrc_cfg.tx_low_threshold = (__u16)flrc_tx_low_thresh;

    /* Seed RF frequency + TX power + PA ramp + Rx boost from module params
     * (clamped). */
    {
        unsigned int f = rf_freq_hz;
        int p = rf_tx_power_dbm;
        unsigned int r = tx_ramp_idx;
        unsigned int b = rx_boost;
        if (f < 1900000000u) f = 1900000000u;
        if (f > 2500000000u) f = 2500000000u;
        if (p > 12)          p = 12;
        if (p < -18)         p = -18;
        if (r > 0x12u)       r = 0x12u;   /* last table 7-21 row (304 us) */
        if (b > 7u) {                     /* rx_boost is 3 bits: never send a malformed byte */
            dev_warn(&spi->dev, "rx_boost=%u out of range, clamping to 7\n", b);
            b = 7u;
        }
        lr2021_dev->rf_freq_hz      = f;
        lr2021_dev->rf_tx_power_dbm = p;
        lr2021_dev->tx_ramp_idx     = (u8)r;
        lr2021_dev->rx_boost        = (u8)b;
    }

    /* Arm the rx waitqueue and tx work struct BEFORE anything that could fire
     * them: the workqueue (which queues tx_work) and the threaded IRQ (which
     * continues the TX chain via lr2021_tx_submit_locked and calls
     * wake_up_interruptible(&rx_waitq)). Plain init calls, can't fail.
     * tx_inflight starts false (kzalloc); tx_lock was init above. */
    init_waitqueue_head(&lr2021_dev->rx_waitq);
    INIT_WORK(&lr2021_dev->tx_work, lr2021_tx_work);

    /* RX frame self-sync: rx_sync is zero-initialised by kzalloc (no votes,
     * sticky offset 0). The offset is decided per packet -- see
     * lr2021_frame_sync.h / lr2021_rx_frame_offset(). */

    /* Allocate ring buffers */
    ret = lr2021_ring_init(lr2021_dev);
    if (ret)
        goto err_ring;

    /* Register character device */
    ret = alloc_chrdev_region(&lr2021_dev->devno, 0, 1, LR2021_DRIVER_NAME);
    if (ret)
        goto err_chrdev;

    cdev_init(&lr2021_dev->cdev, &lr2021_fops);
    lr2021_dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&lr2021_dev->cdev, lr2021_dev->devno, 1);
    if (ret)
        goto err_cdev_add;

    /* Create device class. NOTE: kernel >= 6.4 dropped the THIS_MODULE arg
     * from class_create(); the macro now takes just the name. RPi 6.6 kernels
     * require this single-arg form. */
    lr2021_class = class_create(LR2021_DRIVER_NAME);
    if (IS_ERR(lr2021_class)) {
        ret = PTR_ERR(lr2021_class);
        goto err_class;
    }

    /* device_create() uses the name verbatim -- udev does NOT auto-append the
     * minor number. We pass "%s%d" so the node lands at /dev/radio0 (matching
     * pivid-tx/-rx, the systemd unit's ConditionPathExists=, and the deploy/
     * smoke scripts). Allocating 1 minor starting at 0 means MINOR() == 0. */
    device_create(lr2021_class, NULL, lr2021_dev->devno, NULL,
                  "%s%d", LR2021_DEVICE_NAME, MINOR(lr2021_dev->devno));

    /* GPIO descriptors. Names ("reset", "busy", "dio9") must match the DT
     * overlay's `reset-gpios` / `busy-gpios` / `dio9-gpios` properties
     * (the DT overlay). devm_* tracks them against the SPI device; on driver
     * unbind they're auto-released, so no manual gpiod_put is needed --
     * not even in the error labels below. NRESET is requested OUT_LOW
     * (released, given DT marks it GPIO_ACTIVE_LOW). */
    lr2021_dev->nreset = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(lr2021_dev->nreset)) {
        ret = PTR_ERR(lr2021_dev->nreset);
        dev_err(&spi->dev, "get reset-gpio failed: %d\n", ret);
        goto err_gpio;
    }
    lr2021_dev->busy = devm_gpiod_get(&spi->dev, "busy", GPIOD_IN);
    if (IS_ERR(lr2021_dev->busy)) {
        ret = PTR_ERR(lr2021_dev->busy);
        dev_err(&spi->dev, "get busy-gpio failed: %d\n", ret);
        goto err_gpio;
    }
    lr2021_dev->dio9 = devm_gpiod_get(&spi->dev, "dio9", GPIOD_IN);
    if (IS_ERR(lr2021_dev->dio9)) {
        ret = PTR_ERR(lr2021_dev->dio9);
        dev_err(&spi->dev, "get dio9-gpio failed: %d\n", ret);
        goto err_gpio;
    }

    /* TX workqueue: single-threaded (max_active=1). The worker only primes
     * the IRQ-driven TX chain from idle (claims tx_inflight under tx_lock,
     * submits the first packet under cmd_mutex); the DIO9 IRQ continues the
     * chain. WQ_HIGHPRI keeps it ahead of bulk system_wq work. WQ_MEM_RECLAIM
     * lets the worker make forward progress under memory pressure (we don't
     * allocate on the TX path today, but the flag is cheap insurance and
     * tags the workqueue as latency-critical to the scheduler). */
    lr2021_dev->tx_wq = alloc_workqueue("lr2021_tx",
                                        WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
    if (!lr2021_dev->tx_wq) {
        ret = -ENOMEM;
        dev_err(&spi->dev, "alloc_workqueue failed\n");
        goto err_wq;
    }

    /* Push SPI mode/bits/speed to the controller. */
    ret = lr2021_spi_init(lr2021_dev);
    if (ret) {
        dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
        goto err_spi_init;
    }

    /* Hardware-reset the chip before bring-up so a WARM module reload
     * (rmmod + modprobe, no power cycle) starts from the same clean POR state
     * as a cold boot. MEASURED 2026-06-05: without this, a warm reload leaves
     * the chip in a state where continuous RX overflows the RX FIFO
     * persistently (rx_fifo_overflow climbs ~850/s) even though hw_init
     * reconfigures every register -- the silicon needs the NRESET edge, not
     * just re-issued commands. Cold boot worked only because power-on POR did
     * the reset for us; production deploy.sh reboots, so this was latent.
     * hw_reset is pure GPIO (no SPI, no cmd_mutex); safe here. */
    lr2021_hw_reset(lr2021_dev);

    /* Run the 14-step LR2021 FLRC bring-up. Takes cmd_mutex
     * itself; on failure the chip may be partially configured -- devm
     * cleanup will release NRESET, leaving the line driven low (DT
     * GPIOD_ACTIVE_LOW => OUT_LOW = released), which is correct: a
     * power-cycle on the next probe will re-init from POR. */
    ret = lr2021_hw_init(lr2021_dev);
    if (ret) {
        dev_err(&spi->dev, "hw_init failed: %d\n", ret);
        goto err_hw_init;
    }

    /* Map DIO9 GPIO to a Linux IRQ number, requesting LEVEL_HIGH triggering
     * (overriding the DT `interrupts` default) -- LOAD-BEARING. The DT overlay
     * is intentionally left edge-rising; the driver-side request type is
     * authoritative, so do NOT "reconcile" the overlay to LEVEL_HIGH (some GPIO
     * irqchips reject a request whose type conflicts with a hardwired DT type).
     *
     * DIO9 is an active-high IRQ output: the chip holds it high while ANY
     * masked condition is pending and only releases it when the host clears
     * the IRQ (GetAndClearIrq, done in lr2021_dio9_thread). With EDGE_RISING
     * this deadlocks: IRQF_ONESHOT masks the line from the hardirq until the
     * thread returns, so a NEW condition asserting in the window after the
     * thread's GetAndClearIrq but before unmask leaves DIO9 high with no
     * future rising edge -- the handler never runs again and the line is
     * wedged high forever (observed 2026-06-04: gpio27/dio9 stuck `in hi`, TX
     * wedged with no TX_DONE / RX deaf, on BOTH nodes -- one at boot, one
     * after an RX->TX transition; unrecoverable by NRESET). LEVEL_HIGH is
     * immune: while DIO9 stays high the threaded handler is re-dispatched
     * after each ONESHOT unmask until the source is drained and the line
     * drops, so no edge is ever required. IRQF_ONESHOT stays (the thread does
     * sleeping SPI; the line must stay masked across it).
     *
     * Requested LAST: the IRQ can fire the instant it's installed (DIO9 may
     * already be high after hw_init), and everything it touches (cmd_mutex,
     * tx_lock/tx_inflight, rx_waitq, rx_ring) is now initialized. */
    lr2021_dev->irq = gpiod_to_irq(lr2021_dev->dio9);
    if (lr2021_dev->irq < 0) {
        ret = lr2021_dev->irq;
        dev_err(&spi->dev, "gpiod_to_irq(dio9) failed: %d\n", ret);
        goto err_irq;
    }

    ret = devm_request_threaded_irq(&spi->dev, lr2021_dev->irq,
                                    NULL, lr2021_dio9_thread,
                                    IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
                                    "lr2021-dio9", lr2021_dev);
    if (ret) {
        dev_err(&spi->dev, "request_threaded_irq failed: %d\n", ret);
        goto err_irq;
    }

    /* Watchdog runs at 1 Hz and watches dev->last_dio9_ktime. Armed
     * after the IRQ is registered so the timestamp can actually start
     * advancing; torn down FIRST in remove (before devm_free_irq) so
     * the timer can never race a half-freed device. */
    lr2021_wedge_init(lr2021_dev);

    /* debugfs is best-effort: if it's not mounted/compiled, the calls
     * return an ERR_PTR/NULL that debugfs_create_file tolerates, and we
     * carry on -- observability is non-fatal to the driver. */
    lr2021_debugfs_dir = debugfs_create_dir(LR2021_DRIVER_NAME, NULL);
    debugfs_create_file("rssi_inst", 0444, lr2021_debugfs_dir, NULL,
                        &lr2021_rssi_inst_fops);
    debugfs_create_file("stats", 0444, lr2021_debugfs_dir, NULL,
                        &lr2021_stats_fops);

    pr_info("lr2021 driver loaded\n");
    return 0;

err_irq:
err_hw_init:
err_spi_init:
    destroy_workqueue(lr2021_dev->tx_wq);
err_wq:
err_gpio:
    device_destroy(lr2021_class, lr2021_dev->devno);
    class_destroy(lr2021_class);
err_class:
    cdev_del(&lr2021_dev->cdev);
err_cdev_add:
    unregister_chrdev_region(lr2021_dev->devno, 1);
err_chrdev:
    lr2021_ring_cleanup(lr2021_dev);
err_ring:
    kfree(lr2021_dev->spi_rx_buf);
    kfree(lr2021_dev->spi_tx_buf);
    kfree(lr2021_dev->tx_refill_buf);
    kfree(lr2021_dev);
    return ret;
}

/* NOTE: In modern kernels spi_driver->remove returns void (since v5.18 it was
 * unused, since v6.11 it's strictly void). RPi 6.6 expects void; returning int
 * here is a compile error. */
static void lr2021_remove(struct spi_device *spi) {
    if (lr2021_dev) {
        /* Module-ref discipline: fops.owner + cdev.owner = THIS_MODULE
         * keep the module pinned while /dev/radio0 is held open, so by
         * the time we reach lr2021_remove() no userspace ioctl can fire
         * a fresh queue_work().
         *
         * Teardown order:
         *   1. Release the threaded IRQ FIRST. devm_free_irq() calls
         *      free_irq() under the hood, which synchronously stops the
         *      IRQ thread (free_irq -> __free_irq -> kthread_stop on the
         *      thread_fn worker, plus synchronize_irq for the hardirq).
         *      Without this, the devm auto-release runs AFTER our
         *      kfree(lr2021_dev) below -- and any in-flight DIO9 IRQ
         *      thread would then call lr2021_drain_rx_packet ->
         *      lr2021_ring_rx_push on a freed dev->rx_ring (NULL after
         *      ring_cleanup), giving a NULL deref at the rd_idx load.
         *      Observed 2026-05-30 as "lr2021_ring_rx_push+0x28" oop
         *      right after the "lr2021 driver removed" pr_info.
         *   2. Tear down /dev node + cdev so no NEW opens succeed.
         *   3. cancel_work_sync(tx_work) BEFORE destroy_workqueue --
         *      otherwise destroy_workqueue would flush the worker
         *      mid-iteration into a half-freed device.
         *   4. destroy_workqueue once the worker is provably idle.
         *   5. Free the rings (IRQ thread provably gone, worker idle).
         *   6. Free the device struct LAST. (GPIOs are released by the
         *      SPI core after this function returns.)
         *
         * The wedge watchdog timer is quiesced even before step 1:
         * its callback acquires cmd_mutex and issues SPI commands, so
         * it MUST be stopped before devm_free_irq (which can block on
         * the in-flight IRQ thread that might in turn block on
         * cmd_mutex held by the timer). timer_delete_sync waits for
         * any in-flight callback to return before proceeding. */
        debugfs_remove_recursive(lr2021_debugfs_dir);
        lr2021_debugfs_dir = NULL;
        lr2021_wedge_destroy(lr2021_dev);
        devm_free_irq(&spi->dev, lr2021_dev->irq, lr2021_dev);
        device_destroy(lr2021_class, lr2021_dev->devno);
        class_destroy(lr2021_class);
        cdev_del(&lr2021_dev->cdev);
        unregister_chrdev_region(lr2021_dev->devno, 1);
        cancel_work_sync(&lr2021_dev->tx_work);
        destroy_workqueue(lr2021_dev->tx_wq);
        lr2021_ring_cleanup(lr2021_dev);
        kfree(lr2021_dev->spi_rx_buf);
        kfree(lr2021_dev->spi_tx_buf);
        kfree(lr2021_dev->tx_refill_buf);
        kfree(lr2021_dev);
    }
    pr_info("lr2021 driver removed\n");
}

/* ---------- driver registration ----------------------------------------- */

/* SPI id table: used when the device is registered by board file or via
 * spidev compatibility fallback. The DT overlay does NOT match
 * on this; it matches via .of_match_table below. */
static const struct spi_device_id lr2021_id[] = {
    { "lr2021", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, lr2021_id);

/* Device-tree match table -- this is how the lr2021 node in the DT overlay
 * (compatible = "pivid,lr2021") binds to this driver. Without of_match_table
 * the driver loads but .probe() never fires and /dev/radio0 never appears. */
static const struct of_device_id lr2021_of_match[] = {
    { .compatible = "pivid,lr2021" },
    { }
};
MODULE_DEVICE_TABLE(of, lr2021_of_match);

static struct spi_driver lr2021_driver = {
    .driver = {
        .name           = LR2021_DRIVER_NAME,
        .of_match_table = lr2021_of_match,
    },
    .id_table = lr2021_id,
    .probe    = lr2021_probe,
    .remove   = lr2021_remove,
};

module_spi_driver(lr2021_driver);
