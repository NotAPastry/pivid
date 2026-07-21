#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/ratelimit.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include "lr2021.h"
#include "lr2021_spi.h"
#include "lr2021_commands.h"

/* Post-opcode idle gap for the direct FIFO read (see lr2021_spi.h). Exposed
 * as a module parameter so the misframe fix can be swept on hardware without
 * a rebuild: insmod lr2021.ko fifo_read_delay_us=N. Default LR2021_FIFO_READ_DELAY_US.
 *
 * loadhog A/B (rx_frame_stray under memory-bus contention) measured a U-shaped
 * response: no gap ~29%, ~1.4% at 1 us, rising again past ~50 us. 1 us is the
 * measured minimum. The data phase below can be DMA (one transfer) or PIO
 * (<=88-byte chunks). Both knobs are runtime params so the full matrix is
 * A/B-able without rebuilding once the RF link is restored for live test.
 *
 * fifo_read_pio HW A/B on the live RF link (2026-06-27, RX Pi, interleaved
 * 90 s windows; docs/MEASUREMENTS.md "fifo_read_pio"): PIO does NOT help. Under real production load DMA=1.08%/PIO=3.11% stray
 * (PIO ~3x WORSE -- the CPU-fed chunk seams add their own mid-stream gaps);
 * under heavy loadhog both collapse to ~19% stray (tie). The "PIO drives the
 * residual to ~0" hypothesis is refuted -- DMA (fifo_read_pio=0) stays the
 * default; the knob is kept only as a documented dead-end. */
static unsigned int fifo_read_delay_us = LR2021_FIFO_READ_DELAY_US;
module_param(fifo_read_delay_us, uint, 0644);
MODULE_PARM_DESC(fifo_read_delay_us,
                 "post-opcode CS-held idle gap (us) in the direct RX FIFO read");

/* 0 = data phase is one DMA transfer (bcm2835 DMA, >96 bytes); 1 = data phase
 * is split into <=LR2021_FIFO_READ_CHUNK PIO transfers (no DMA-setup jitter). */
static bool fifo_read_pio;
module_param(fifo_read_pio, bool, 0644);
MODULE_PARM_DESC(fifo_read_pio,
                 "read RX FIFO data phase via PIO chunks instead of one DMA transfer");

/* Status byte count returned at the start of every response/data frame
 * (datasheet section 5.4.1.1 / fig 5-3 / fig 5-4): the chip clocks out
 * Stat(15:0) during the first two MISO bytes of every read frame. We
 * discard them; the caller sees only the payload bytes. */
#define LR2021_STATUS_BYTES 2

/* Bump dev->errors_spi safely from any context. The SPI completion
 * callback can run in softirq context on some controllers, so use
 * _irqsave even though most callers are in process context. */
static void lr2021_inc_spi_errors(LR2021_Device *dev) {
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, flags);
    dev->errors_spi++;
    spin_unlock_irqrestore(&dev->lock, flags);
}

int lr2021_spi_init(LR2021_Device *dev) {
    dev->spi->mode          = LR2021_SPI_MODE;
    dev->spi->bits_per_word = LR2021_SPI_BITS_PER_WORD;
    dev->spi->max_speed_hz  = LR2021_SPI_HZ;
    return spi_setup(dev->spi);
}

int lr2021_spi_wait_busy_low(LR2021_Device *dev, int timeout_ms) {
    ktime_t deadline;

    /* Defensive: probe wires dev->busy before any command is issued; until
     * then commands are never sent, but keep this safe in case it ever is called
     * pre-init (test harnesses, manual ioctl during early boot, etc.). */
    if (!dev->busy)
        return 0;

    deadline = ktime_add_ms(ktime_get(), timeout_ms);

    /* IMP-2b: most BUSY intervals after a command are shorter than a sleep
     * quantum, so a blind usleep_range(10,50) donates 10-50 us per SPI command
     * (2-3 commands per packet on the RX drain path). Spin for the first
     * ~20 us -- process context, cmd_mutex held, so the only cost is this
     * core -- then fall back to a tighter usleep_range(2,10) for the rare
     * long BUSY (calibration, mode transitions). */
    {
        ktime_t spin_end = ktime_add_us(ktime_get(), 20);
        while (gpiod_get_value(dev->busy) != 0) {
            if (ktime_after(ktime_get(), spin_end))
                break;
            cpu_relax();
        }
    }
    while (gpiod_get_value(dev->busy) != 0) {
        if (ktime_after(ktime_get(), deadline)) {
            lr2021_inc_spi_errors(dev);
            return -ETIMEDOUT;
        }
        usleep_range(2, 10);
    }
    return 0;
}

int lr2021_spi_cmd(LR2021_Device *dev,
                   uint16_t opcode,
                   const uint8_t *args, size_t args_len,
                   uint8_t *resp, size_t resp_len) {
    size_t cmd_len       = 2 + args_len;
    size_t resp_frame    = LR2021_STATUS_BYTES + resp_len;
    uint8_t *tx          = dev->spi_tx_buf;
    uint8_t *rx          = dev->spi_rx_buf;
    int      ret;

    /* Caller MUST hold dev->cmd_mutex across the entire two-frame
     * request/response sequence -- which also makes the shared spi_tx_buf /
     * spi_rx_buf scratch safe (reused sequentially, never concurrently). See
     * lr2021_spi.h for the locking contract.
     *
     * The scratch buffers are pre-allocated DMA-safe regions (probe-time
     * kmalloc, LR2021_SPI_SCRATCH_*), replacing a per-call kzalloc on the hot
     * path: spi_sync may require a DMA-safe region on controllers that
     * bounce-buffer, and stack buffers are not guaranteed DMA-safe. Bounds-
     * guard so a future larger command is caught here rather than silently
     * overrunning the buffer. */
    if (cmd_len > LR2021_SPI_SCRATCH_TX || resp_frame > LR2021_SPI_SCRATCH_RX) {
        dev_err_ratelimited(&dev->spi->dev,
            "SPI cmd 0x%04x too large for scratch (cmd=%zu resp=%zu)\n",
            opcode, cmd_len, resp_frame);
        return -EINVAL;
    }

    tx[0] = (uint8_t)(opcode >> 8);
    tx[1] = (uint8_t)(opcode & 0xff);
    if (args && args_len > 0)
        memcpy(tx + 2, args, args_len);

    /* The chip may still be processing a prior command (e.g., a calibration
     * triggered before the previous return). Block until it's idle. */
    ret = lr2021_spi_wait_busy_low(dev, LR2021_BUSY_TIMEOUT_MS);
    if (ret)
        return ret;

    ret = spi_write(dev->spi, tx, cmd_len);
    if (ret) {
        lr2021_inc_spi_errors(dev);
        return ret;
    }

    if (resp && resp_len > 0) {
        /* Datasheet fig 5-3: response is the SECOND frame, after BUSY has gone
         * low to signal the chip prepared the data. This separate (NSS-toggling)
         * read frame is the intended command-response pattern -- distinct from
         * lr2021_spi_fifo_read's deferred path, where a bare read re-clocks
         * 0x0000 as a fresh opcode (the chip latches the just-issued GetRx*
         * opcode and returns its data on the next frame). For a command here the
         * opcode+args were already clocked in the write frame above, so this read
         * just retrieves the prepared Stat+payload. */
        ret = lr2021_spi_wait_busy_low(dev, LR2021_BUSY_TIMEOUT_MS);
        if (ret)
            return ret;

        ret = spi_read(dev->spi, rx, resp_frame);
        if (ret) {
            lr2021_inc_spi_errors(dev);
            return ret;
        }

        /* First 2 MISO bytes are Stat(15:0); skip them, copy the
         * caller-requested payload. */
        memcpy(resp, rx + LR2021_STATUS_BYTES, resp_len);

        /* Those 2 Stat bytes (rx[0]=Stat(15:8), rx[1]=Stat(7:0), datasheet
         * Table 6-38) are free observability -- decode rather than only
         * discard. For a read, Stat is the read's own status (CMD_DAT on
         * success); a CMD_FAIL/CMD_PERR flags a rejected read. ChipMode is the
         * chip's live mode, stashed for the heartbeat. Done ONLY here, on the
         * response frame we already clock -- making the command frame duplex to
         * also catch write-only commands measurably perturbed the >256 B RX
         * drain timing (2026-06-05 rx_fifo_overflow storm). Write-only command
         * rejections are instead caught via the CmdError IRQ. AN1200.104 §2.1. */
        WRITE_ONCE(dev->last_chip_mode, lr2021_stat_chip_mode(rx[1]));
        if (lr2021_stat_cmd_failed(rx[0])) {
            atomic_inc(&dev->cmd_status_err);
            dev_warn_ratelimited(&dev->spi->dev,
                "cmd 0x%04x rejected: Stat cmd_status=%u chip_mode=%u\n",
                opcode, lr2021_stat_cmd_status(rx[0]),
                lr2021_stat_chip_mode(rx[1]));
        }
    }

    /* INVARIANT: a write-only command (resp == NULL) returns here WITHOUT
     * re-polling BUSY -- the chip may still be processing. This is safe only
     * because the NEXT lr2021_spi_cmd begins with wait_busy_low above, so
     * back-to-back commands serialise on the chip's BUSY line. A caller that
     * needs the chip's state to have settled before a NON-SPI action (or before
     * assuming the command's effect is live) must issue a following command or
     * poll BUSY itself; this function returning is not a completion signal. */
    return 0;
}

int lr2021_spi_fifo_write(LR2021_Device *dev,
                          const uint8_t *data, size_t len) {
    size_t   total = 2 + len;
    uint8_t *tx    = dev->spi_tx_buf;
    int      ret;

    /* Caller MUST hold dev->cmd_mutex (also guards the shared spi_tx_buf
     * scratch). See lr2021_spi.h. Bounds-guard the pre-allocated scratch. */
    if (total > LR2021_SPI_SCRATCH_TX) {
        dev_err_ratelimited(&dev->spi->dev,
            "SPI fifo_write len %zu exceeds scratch\n", len);
        return -EINVAL;
    }

    tx[0] = (uint8_t)(LR2021_CMD_WRITE_TX_FIFO >> 8);
    tx[1] = (uint8_t)(LR2021_CMD_WRITE_TX_FIFO & 0xff);
    memcpy(tx + 2, data, len);

    ret = lr2021_spi_wait_busy_low(dev, LR2021_BUSY_TIMEOUT_MS);
    if (ret)
        return ret;

    /* Datasheet fig 5-5: direct-write single frame -- opcode + payload
     * in one SPI transaction; no separate response. */
    ret = spi_write(dev->spi, tx, total);
    if (ret)
        lr2021_inc_spi_errors(dev);

    return ret;
}

int lr2021_spi_fifo_read(LR2021_Device *dev, uint8_t *data, size_t len) {
    /* xfer[0] = opcode(2)+gap; then 1 (DMA) or up to ceil(len/CHUNK) (PIO)
     * data transfers. len <= 256 -> at most 3 PIO chunks -> 4 transfers. */
    uint8_t *tx = dev->spi_tx_buf;
    uint8_t *rx = dev->spi_rx_buf;
    struct spi_transfer xfer[1 + 4];
    struct spi_message  msg;
    size_t   off, n;
    int ret;

    /* Caller MUST hold dev->cmd_mutex (also guards the shared spi_tx_buf /
     * spi_rx_buf scratch). See lr2021_spi.h. Bounds-guard the pre-allocated
     * scratch: tx needs opcode+len, rx needs len. */
    if (2 + len > LR2021_SPI_SCRATCH_TX || len > LR2021_SPI_SCRATCH_RX) {
        dev_err_ratelimited(&dev->spi->dev,
            "SPI fifo_read len %zu exceeds scratch\n", len);
        return -EINVAL;
    }

    /* Datasheet fig 5-4 / §5.4.1.3: the direct radio-FIFO read is a SINGLE
     * SPI frame -- MISO returns Stat(15:0) then the FIFO bytes "byte-by-byte
     * immediately after the opcode has been parsed." Without an idle gap after
     * the opcode the chip emits a stale stray byte before Data0, byte-shifting
     * the packet (project_rx_misframe_fec_fabrication; ~29% under loadhog).
     *
     * Fix: ONE frame [opcode][idle gap][data], CS held low across the whole
     * message (cs_change stays 0) so the chip readies Data0 -> leading MISO
     * bytes are deterministically the 2 Stat bytes. The gap is fifo_read_delay_us.
     * The data phase is either one DMA transfer or PIO chunks (fifo_read_pio);
     * PIO avoids the bcm2835 DMA-setup jitter that makes the effective idle
     * vary under load. lr2021_frame_sync stays as a backstop for any residual.
     *
     * Buffer layout: tx = [op_hi][op_lo][len dummy zero bytes]; rx receives
     * the len FIFO data bytes (the 2 Stat bytes are in the opcode xfer). */
    tx[0] = (uint8_t)(LR2021_CMD_READ_RX_FIFO >> 8);
    tx[1] = (uint8_t)(LR2021_CMD_READ_RX_FIFO & 0xff);
    /* MOSI dummy bytes for the data phase. The chip ignores MOSI once the read
     * opcode is parsed, but zero them so the clocked-out MOSI is deterministic
     * -- preserving the prior kzalloc'd-zero behaviour now that the scratch
     * buffer is reused across calls (a previous write left stale bytes here). */
    memset(tx + 2, 0, len);

    memset(xfer, 0, sizeof(xfer));
    spi_message_init(&msg);

    /* Opcode transfer: clock the 2 opcode bytes (MISO Stat discarded), then
     * hold CS low for the post-opcode gap so the chip readies Data0. */
    xfer[0].tx_buf      = tx;
    xfer[0].len         = 2;
    xfer[0].delay.value = fifo_read_delay_us;
    xfer[0].delay.unit  = SPI_DELAY_UNIT_USECS;
    spi_message_add_tail(&xfer[0], &msg);

    /* Data phase: MISO = FIFO bytes. cs_change left 0 throughout (one frame). */
    n = 1;
    off = 0;
    do {
        size_t chunk = len - off;
        if (fifo_read_pio && chunk > LR2021_FIFO_READ_CHUNK)
            chunk = LR2021_FIFO_READ_CHUNK;
        xfer[n].tx_buf = tx + 2 + off;
        xfer[n].rx_buf = rx + off;
        xfer[n].len    = chunk;
        spi_message_add_tail(&xfer[n], &msg);
        off += chunk;
        n++;
    } while (off < len);

    ret = lr2021_spi_wait_busy_low(dev, LR2021_BUSY_TIMEOUT_MS);
    if (ret)
        return ret;

    ret = spi_sync(dev->spi, &msg);
    if (ret) {
        lr2021_inc_spi_errors(dev);
        return ret;
    }

    memcpy(data, rx, len);
    return 0;
}

/* No async TX path at this layer. Async-from-userspace is provided by
 * the TX workqueue worker (lr2021_tx.c), which calls lr2021_spi_fifo_write +
 * lr2021_spi_cmd(SET_TX, ...) under cmd_mutex and continues the chain from the
 * DIO9 TX_DONE IRQ. See lr2021_tx.c for the full lifecycle. */
