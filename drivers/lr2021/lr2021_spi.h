#ifndef LR2021_SPI_H
#define LR2021_SPI_H

#include <linux/types.h>

struct LR2021_Device;

/* BUSY-pin poll timeout (ms). Any LR2021 command that doesn't return within
 * this window is treated as a hardware fault: caller gets -ETIMEDOUT and
 * dev->errors_spi is incremented. 100 ms is several orders of magnitude
 * above the datasheet's stated command processing latency (sub-ms for the
 * commands this driver uses); only used as a hang/wedge sentinel. */
#define LR2021_BUSY_TIMEOUT_MS 100

/* SPI bus parameters. SPI mode 0 per datasheet section 5.5. 16 MHz is the
 * highest rate the LR2021 supports for command/FIFO frames. */
#define LR2021_SPI_HZ          (16 * 1000 * 1000)
#define LR2021_SPI_MODE        SPI_MODE_0
#define LR2021_SPI_BITS_PER_WORD 8

/* Post-opcode idle gap (NSS held low, no clock) inside the direct-read FIFO
 * frame, so the chip has fetched Data0 before we clock the data phase.
 *
 * The stray leading byte that byte-shifts RX packets
 * (project_rx_misframe_fec_fabrication) is a chip Data0-fetch latency issue,
 * NOT a DMA artifact. Evidence (loadhog memory-bus contention, measured via
 * the driver's rx_frame_stray counter):
 *   - split [opcode][10us gap][data]          : ~1.9% strays under load
 *   - single full-duplex read (no gap) @ 4MHz : ~29% strays
 *   - PIO-chunked read (no gap)        @16MHz : ~29% strays
 * The discriminating variable is the post-opcode idle gap, not the clock and
 * not DMA: with the gap the chip reliably presents Data0; without it ~29% of
 * frames get a stray. The gap-vs-stray curve is U-shaped -- too long is as bad
 * as too short, because a long DMA-setup gap lets the chip advance past Data0.
 *
 * On-air sweep @2.03GHz, 16MHz SPI, ~217 pkt/s, measured via seq_drop (= real
 * misframe rate, since RF crc=0) and the rx_frame_stray counter:
 *   delay=1us  pio=N : 0.02% misframe, 0.015% stray   <-- optimum
 *   delay=10us pio=N : 4.5%  misframe, 2.6%  stray
 *   delay=20us pio=N : 2.6%  misframe
 *   delay=30us pio=Y : 4.9%  misframe, 16.9% stray
 * 1us was the curve minimum at 217 pkt/s; PIO (fifo_read_pio) is unnecessary
 * and slightly worse. Both stay runtime-tunable via module params for
 * re-sweeping on other links/clocks.
 *
 * RE-SWEEP 2026-06-29 (RX Pi at ~390 pkt/s under caster load, measured via the
 * rx_frame_ambig counter, interleaved A/B): the minimum shifted to ~3us
 * (ambig 1.08% @3us vs 1.60% @1us; crc=0 throughout). Higher packet rate +
 * host load widened the realized-gap jitter, moving the optimum out. Default
 * bumped 1->3; deployment also pins it via /etc/modprobe.d/lr2021.conf. Re-sweep
 * if the link rate, SPI clock, or host load changes materially. */
#define LR2021_FIFO_READ_DELAY_US 3

/* Max PIO chunk for the optional PIO data-phase path (module param
 * fifo_read_pio). bcm2835 uses DMA only above BCM2835_SPI_DMA_MIN_LENGTH = 96
 * bytes, so <=88-byte transfers stay in PIO -- no DMA-setup jitter. */
#define LR2021_FIFO_READ_CHUNK 88

/* One-time SPI bus configuration (mode/speed/bpw). Called from .probe(). */
int lr2021_spi_init(struct LR2021_Device *dev);

/* Block until the LR2021's BUSY GPIO goes low, or until timeout_ms elapses.
 * Returns 0 on success, -ETIMEDOUT on timeout. Sleeps in usleep_range(10, 50)
 * intervals; safe to call from threaded IRQ context but never from atomic. */
int lr2021_spi_wait_busy_low(struct LR2021_Device *dev, int timeout_ms);

/* ---- Locking contract ------------------------------------------------- *
 *
 * All three primitives below (lr2021_spi_cmd, lr2021_spi_fifo_write,
 * lr2021_spi_fifo_read) require the caller to hold `dev->cmd_mutex`.
 *
 * The mutex was originally taken inside each primitive, but that made
 * higher-level transactions (worker's SetFlrcPacketParams + fifo_write +
 * SetTx; IRQ handler's GetAndClearIrq + GetRxPktLength + fifo_read)
 * either deadlock on the non-recursive mutex or fragment into multiple
 * separately-locked steps that another waiter could interleave between.
 *
 * Caller-owned locking lets each composite operation define its own
 * transaction boundary while the primitives stay simple wire ops.
 *
 * Callers and their boundaries:
 *   - lr2021_tx.c worker        : holds mutex across the submit -- (optional)
 *                                 SetFlrcPacketParams + fifo_write + SetTx --
 *                                 then drops it. TX completion arrives
 *                                 asynchronously via the DIO9 TX_DONE IRQ
 *                                 (which chains the next packet), not a
 *                                 wait_for_completion in the worker.
 *   - lr2021_irq.c dio9 thread  : holds mutex across GetAndClearIrq +
 *                                 (optional) GetRxPktLength + fifo_read.
 *   - lr2021_core.c ioctls      : take mutex inside each apply_* helper.
 *   - lr2021_init.c hw_init     : takes mutex for the whole 14-step
 *                                 bring-up so nothing interleaves.
 * --------------------------------------------------------------------- */

/* Two-frame command (datasheet section 5.5):
 *   frame 1: [opcode_hi, opcode_lo, args[0..args_len-1]]
 *   wait BUSY low
 *   frame 2: read resp_len bytes (skipped if resp == NULL or resp_len == 0)
 *
 * Caller MUST hold dev->cmd_mutex across the call.
 *
 * Returns 0 on success, negative errno on SPI/timeout failure. */
int lr2021_spi_cmd(struct LR2021_Device *dev,
                   uint16_t opcode,
                   const uint8_t *args, size_t args_len,
                   uint8_t *resp, size_t resp_len);

/* Single-frame FIFO direct access (datasheet section 5.6.1).
 *
 * Wire format: opcode (16-bit, MSB first) followed by `len` payload bytes.
 * The LR2021's RxFifo opcode 0x0001 returns `len` bytes on MISO during the
 * payload phase; the TxFifo opcode 0x0002 consumes `len` bytes from MOSI.
 *
 * Caller MUST hold dev->cmd_mutex across each call.
 *
 * The caller decides `len` from GetRxPktLength / packet size; the chip uses
 * the SPI transaction length to bound the payload (no separate response). */
int lr2021_spi_fifo_write(struct LR2021_Device *dev,
                          const uint8_t *data, size_t len);
int lr2021_spi_fifo_read (struct LR2021_Device *dev,
                          uint8_t *data, size_t len);

#endif /* LR2021_SPI_H */
