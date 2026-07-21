#ifndef LR2021_H
#define LR2021_H

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include "radio_ioctl.h"     /* RADIO_RING_BYTES, RADIO_RING_DATA_MASK, ... */
#include "lr2021_ring.h"     /* struct lr2021_ring + kernel ring API */
#include "lr2021_frame_sync.h" /* struct lr2021_frame_sync + per-packet offset */
#include "lr2021_flrc_cfg.h" /* radio_flrc_config + pure param encoders */
#include "lr2021_status.h"   /* pure Stat / GetErrors / GetFlrcRxStats decoders */

#define LR2021_DRIVER_NAME "lr2021"
#define LR2021_DEVICE_NAME "radio"

/* Watchdog threshold: in RADIO_MODE_RX, normal inter-DIO9 gaps under load
 * are ~2-3 ms (FLRC at 2.6 Mbps fills 255-byte airtime in ~1 ms). Inter-
 * frame gaps reach ~33 ms (rpicam idle between H.264 frames at 30 fps).
 * 250 ms is well above any normal gap including stream pauses; reaching
 * this threshold means the chip stopped generating RxDone entirely. */
#define LR2021_WEDGE_GRACE_MS 250

/* TX wedge grace: max wall time a packet may be in flight (SetTx issued,
 * TX_DONE not seen) before we declare the chip wedged and recover. A 511 B
 * FLRC packet is ~2.5 ms on air; refill + scheduling jitter add slack. 50 ms
 * matches the old blocking-worker timeout and is ~20x worst-case airtime, so
 * it only fires on a genuine hang, never on a slow-but-live TX. */
#define LR2021_TX_WEDGE_GRACE_MS 50

/* LR2021 TX/RX FIFO is 256 bytes; also the max payload per ReadRadioRxFifo /
 * WriteRadioTxFifo SPI frame. TX refill and RX chunked-read both cap to this. */
#define LR2021_FIFO_CHUNK_BYTES 256

/* Pre-allocated DMA-safe scratch buffers for the SPI leaf ops (lr2021_spi.c),
 * replacing a per-call kzalloc on the hot path. Sized for the largest single
 * SPI frame: the 2-byte opcode plus one 256 B FIFO chunk. One buffer per
 * direction (spi_tx_buf / spi_rx_buf) is shared across spi_cmd / fifo_write /
 * fifo_read and is safe ONLY because every SPI leaf op runs under cmd_mutex
 * (the sole SPI serializer): the buffers are reused sequentially, never
 * concurrently. If a non-cmd_mutex SPI path is ever added, this invariant
 * breaks -- they must then become per-context (see the locking contract in
 * lr2021_spi.h). The leaf ops bounds-check against these sizes. */
#define LR2021_SPI_SCRATCH_TX (2 + LR2021_FIFO_CHUNK_BYTES)
#define LR2021_SPI_SCRATCH_RX (2 + LR2021_FIFO_CHUNK_BYTES)

/* RX drain buffer size: the RX configured max pld_len (511) + 1 for the
 * possible leading stray. Lives in the device struct (single-owner: drained
 * only from the DIO9 threaded IRQ, sequential, no lock). */
#define LR2021_RX_PACKET_BUF 512

/* RxFifo-high watermark (bytes), LR2021_RX_HIGH_THRESHOLD. The chip's RX FIFO
 * is 256 B; a >256 B FLRC payload (up to 511) overflows it unless the host
 * reads bytes out WHILE the packet is still being received (datasheet §5.3.1).
 * We enable the RxFifo-high IRQ at this threshold: it fires once the FIFO fill
 * level REACHES this many bytes, and the DIO9 handler drains them into the
 * accumulator before the FIFO fills. 128 = half the FIFO, leaving ~128 B of
 * slack (~0.4 ms at the 2.6 Mbps FLRC info rate) for the host's SPI read
 * latency before overflow -- the RX-side analogue of the TX refill's
 * tx_low_threshold. The two are NOT the same value and serve opposite FIFO
 * edges: this RX threshold is hardcoded at 128 (deeper margin, because the RX
 * GetRxFifoLevel + fifo_read round trip is the larger latency constraint),
 * whereas the TX side's slack depends on the configurable tx_low_threshold
 * (flrc_cfg / module param). The constant itself now lives in lr2021_flrc_cfg.h
 * so the pure ConfigFifoIrq encoder (lr2021_flrc_encode_fifo_irq) is
 * self-contained; it is pulled in here via that header's include above. */

/* Named struct so it's referenceable as both `struct LR2021_Device` and
 * `LR2021_Device` (lr2021_ring.h forward-declares it). */
typedef struct LR2021_Device {
    struct spi_device *spi;
    struct gpio_desc  *nreset, *busy, *dio9;
    struct cdev        cdev;
    dev_t              devno;
    atomic_t           is_open;     /* 0 = closed, 1 = open. Exclusive: the char
                                     * device fronts one SPSC ring pair; a 2nd
                                     * opener would mmap the same ring and
                                     * corrupt framing. */
    int                irq;
    spinlock_t         lock;        /* protects stats counters below */
    struct mutex       cmd_mutex;   /* serializes multi-frame SPI sequences */

    /* DMA-safe SPI scratch, owned by cmd_mutex (probe-time kmalloc, freed in
     * remove). The SPI leaf ops (lr2021_spi.c) clock commands / FIFO data
     * through these instead of a per-call kzalloc -- removing a sleeping
     * GFP_KERNEL allocation, its -ENOMEM branch, and the per-packet allocator
     * latency variance from the hot path. kmalloc'd so they sit in the kernel
     * direct map (DMA-safe for the bcm2835 >96 B SPI DMA path), like
     * tx_refill_buf. Reused sequentially under cmd_mutex; sized
     * LR2021_SPI_SCRATCH_* (see above). */
    u8                *spi_tx_buf;  /* opcode + args/payload (MOSI)            */
    u8                *spi_rx_buf;  /* Stat + response / FIFO data (MISO)      */

    /* Ring buffers. Each ring's mmap'd indices (wr_idx, rd_idx) live INSIDE
     * struct lr2021_ring, not in this device struct. Allocated in
     * lr2021_ring_init; freed in lr2021_ring_cleanup. */
    struct lr2021_ring *tx_ring;
    struct lr2021_ring *rx_ring;

    /* Mode + bind config. `mode` is updated from ioctl context
     * under cmd_mutex. */
    unsigned int       mode;          /* RADIO_MODE_* */
    uint32_t           sync_word;

    /* FLRC runtime config. Set at probe to the fastest-default values, then
     * overwritten by RADIO_IOCTL_SET_FLRC_CONFIG. Will be read by the TX submit
     * path (a later Phase-B task wires the fixed-pad vs dynamic-reprogram
     * decision off mode + pld_len) and the IRQ refill path (tx_low_threshold);
     * as of this commit the TX path still uses the static LR2021_FLRC_PLD_LEN.
     * Updated only from ioctl context under cmd_mutex; the TX submit reads it
     * under cmd_mutex too, so no extra lock. */
    struct radio_flrc_config flrc_cfg;

    /* RF frequency, TX power, PA ramp and Rx boost. Set at probe from module
     * params (clamped to HF band [1.9e9..2.5e9] Hz, [-18..+12] dBm, ramp table
     * 0x00..0x12, boost 0..7); consumed by lr2021_hw_init to compute CalibFE /
     * SetRfFrequency / SetTxParams / SetRxPath bytes at bring-up. */
    u32                rf_freq_hz;      /* carrier Hz (module param rf_freq_hz)   */
    int                rf_tx_power_dbm; /* TX power dBm (module param)            */
    u8                 tx_ramp_idx;     /* PA ramp index, table 7-21 (module param) */
    u8                 rx_boost;        /* SetRxPath boost 0..7 (module param)    */

    /* RX completion notification for poll/select/epoll on /dev/radio0.
     * Woken by the DIO9 IRQ handler after pushing into rx_ring. */
    wait_queue_head_t  rx_waitq;

    /* RX SPI-frame self-sync. The direct-read FIFO frame can carry a stray
     * leading byte before the real payload (a 16 MHz SPI timing artifact; the
     * post-opcode delay in lr2021_spi_fifo_read removes it at the source). We
     * read pkt_len+1 bytes and decide the offset (0 or 1) PER PACKET -- not
     * locked once per load, since the stray count varies packet-to-packet.
     * Touched only from the DIO9 IRQ thread (single owner) -- no lock.
     * See lr2021_frame_sync.h and project_rx_misframe_fec_fabrication. */
    struct lr2021_frame_sync rx_sync;

    /* Frame-offset decision diagnostics (debugfs, non-ABI). Counted per
     * CRC-good received packet at the frame-sync decision point, so the rate
     * is RF-independent (CRC-failed packets never reach the decision). These
     * directly measure the 16 MHz SPI stray-byte phenomenon at its source:
     *   rx_frame_stray = unambiguous offset-1 decisions (stray detected)
     *   rx_frame_ambig = ambiguous decisions (both/neither header valid) --
     *                    the residual-misframe risk surface (a hidden stray
     *                    here is silently followed to the dominant offset).
     * Bumped from the IRQ thread; atomic so the debugfs reader is safe. */
    atomic_t           rx_frame_total;
    atomic_t           rx_frame_stray;
    atomic_t           rx_frame_ambig;

    /* Multi-packet stream walker diagnostics (debugfs, non-ABI). A coalesced
     * RX_DONE (second packet completed before GetAndClearIrq) makes
     * GetRxPktLength describe the LAST packet, not the FIFO head; the walker
     * (lr2021_rx_walk.h) frames packets by their in-band headers instead.
     *   rx_walk_extra = packets recovered beyond the first in one RX_DONE
     *                   service (== coalesce events handled; the old path
     *                   misframed these, HW 0.34-1.19/s).
     *   rx_walk_drops = accumulated bytes abandoned because the stream head
     *                   was not in-band framable mid-walk (garbage after a
     *                   CRC-passed batch, or a DYN record at a non-pkt_len
     *                   position) -- the residual, previously-silent loss. */
    atomic_t           rx_walk_extra;
    atomic_t           rx_walk_drops;

    /* TX async path: IRQ-driven pipelined state machine. The worker primes
     * the chain from idle; the threaded DIO9 IRQ continues it on TX_DONE.
     *   tx_lock      : spinlock protecting tx_inflight + the start/continue
     *                  decision. Held briefly, NEVER across SPI. Lock order:
     *                  tx_lock -> (release) -> cmd_mutex. cmd_mutex is NEVER
     *                  taken while holding tx_lock (it sleeps).
     *   tx_inflight  : true while a packet is on the wire (SetTx issued,
     *                  TX_DONE not yet seen). Invariant "TX never goes idle
     *                  with a non-empty ring" asserted under tx_lock.
     *   last_submit_ktime : ktime of the most recent SetTx; the wedge watchdog
     *                  (lr2021_wedge.c, Task B8) recovers if tx_inflight and
     *                  now - last_submit_ktime > grace. Stamped under cmd_mutex
     *                  by submit_locked; read via READ_ONCE by the watchdog. */
    struct workqueue_struct *tx_wq;
    struct work_struct       tx_work;
    spinlock_t               tx_lock;
    bool                     tx_inflight;
    ktime_t                  last_submit_ktime;
#ifdef TXPERF
    /* TXPERF (debug profiling, compiled out by default): ktime stamped at the
     * TX_DONE IRQ that triggers the next submit, so submit_locked can measure
     * air (last_submit->TX_DONE) and gap (TX_DONE->next submit start, which the
     * IRQ-driven chain collapses toward 0). */
    ktime_t                  tx_done_ktime;
#endif

    /* Mid-TX FIFO refill cursor (cfg.pld_len > 256 path). The current packet's
     * full on-wire byte count and how many of those bytes have been written to
     * the chip TX FIFO so far. submit_locked writes the first chunk and sets
     * tx_refill_total/tx_refill_done; the TxFifo-low IRQ writes subsequent
     * chunks until done == total. Single-owner under cmd_mutex. The bytes live
     * in tx_refill_buf (kmalloc'd, DMA-safe). */
    u8     *tx_refill_buf;        /* kmalloc'd LR2021_RING_MAX_RECORD bytes
                                    * (>= 511 max payload)                    */
    u16     tx_refill_total;      /* total on-wire bytes for this packet      */
    u16     tx_refill_done;       /* bytes already pushed to the TX FIFO      */

    /* Dynamic-mode (cfg.mode==0) SetFlrcPacketParams.pld_len cache. submit_locked
     * reprograms the on-air length per packet; the FEC packer emits mostly
     * identical-length pairs, so we skip the ~40-48us SPI reprogram when the
     * length is unchanged. Single-owner under cmd_mutex. Invalidated whenever
     * the params are reprogrammed out-of-band (lr2021_apply_flrc_config, the
     * FLRC probe). kzalloc'd -> starts {0, false} = first packet always programs. */
    u16     cached_tx_len;        /* last pld_len programmed via SetFlrcPacketParams */
    bool    tx_len_valid;         /* cached_tx_len reflects the chip's state    */

    u8  rx_drain_buf[LR2021_RX_PACKET_BUF];  /* RX packet drain (DIO9 IRQ
                                              * single-owner; no lock). */

    /* Mid-RX FIFO drain cursor (pld_len > 256 path, the symmetric counterpart
     * to tx_refill_done). For a >256 B FLRC packet the 256 B RX FIFO overflows
     * unless the host reads it out during reception (datasheet §5.3.1); the
     * RxFifo-high IRQ drains the FIFO into rx_drain_buf and advances this offset.
     * RX_DONE then reads only the REMAINING bytes to reach pkt_len+1 and resets
     * this to 0. For <=256 B packets no RxFifo-high fires, so this stays 0 and
     * RX_DONE reads the whole packet exactly as before. Touched only from the
     * DIO9 IRQ thread (single owner, like rx_sync above) -- no lock. */
    u16     rx_drain_off;        /* raw FIFO bytes accumulated for the
                                  * in-progress RX packet (0 = none yet)     */

    /* Stats. The uint64_t group is updated from process context (worker,
     * ioctl, threaded IRQ) under `lock`. */
    uint64_t           packets_tx, packets_rx;
    uint64_t           bytes_tx, bytes_rx;        /* payload throughput */
    uint64_t           errors_spi, errors_irq;    /* errors_irq = IRQ-path
                                                     hard errors only */
    uint64_t           rx_ring_full_drops;        /* host overrun (ring full) */
    uint64_t           tx_runt_drops;             /* TX records dropped for
                                                    * pld_len < FLRC 6 B min
                                                    * (IMP-8; 0 in production) */

    /* RF/link error breakdown. Promoted from function-static atomics in
     * lr2021_dio9_thread so GET_STATS + debugfs can read them. Bumped from
     * the IRQ thread without `lock` (atomic is self-sufficient). */
    atomic_t           crc_count, sync_count, len_count, tmout_count;
    atomic_t           crc_drops;                 /* RX_DONE dropped on CRC fail */

    /* FIFO-flag diagnostics. Bumped per-IRQ from the threaded handler
     * after reading GetFifoIrqFlags; atomic so the heartbeat printout
     * outside cmd_mutex can read them safely. */
    atomic_t           rx_fifo_overflow_count;
    atomic_t           tx_fifo_underflow_count;

    /* Command-status (Stat) diagnostics. lr2021_spi_cmd captures the 2 Stat
     * bytes the chip clocks out with every command frame (datasheet Table
     * 6-38) and decodes CommandStatus: cmd_status_err counts CMD_FAIL/CMD_PERR
     * (the *previous* command was rejected -- the Stat in frame N reflects
     * command N-1, so attribution is one-late but the count is exact). This is
     * the per-command realisation of AN1200.104 §2.1's "read Stat to confirm
     * the command succeeded". last_chip_mode is the chip's self-reported mode
     * (ChipMode field) from the most recent command -- SLEEP/RC/XOSC/FS/Rx/Tx
     * per lr2021_status.h. atomic + WRITE_ONCE keep the lockless readers
     * (heartbeat/debugfs, outside cmd_mutex) coherent. */
    atomic_t           cmd_status_err;
    u8                 last_chip_mode;

    /* Chip-side error + FLRC RX statistics, polled from the DIO9 thread under
     * cmd_mutex (single writer): GetErrors when an ERROR/CmdError IRQ is
     * pending, GetFlrcRxStats once per ~second while in RX mode. These give
     * ground truth independent of the host's framing -- e.g. flrc_rxs_crc is
     * the silicon's own CRC-fail count, separate from the host crc_count.
     * Readers (heartbeat/debugfs) use READ_ONCE; chip_error_stat accumulates
     * (OR) every ErrorStat bit seen since boot. */
    atomic_t           chip_err_count;   /* IRQ Error    (bit 16) occurrences */
    atomic_t           cmd_err_count;    /* IRQ CmdError (bit 17) occurrences */
    u16                chip_error_stat;  /* OR of GetErrors ErrorStat bits    */
    u16                flrc_rxs_pkt;     /* GetFlrcRxStats pkt_rx              */
    u16                flrc_rxs_crc;     /* GetFlrcRxStats crc_error          */
    u16                flrc_rxs_len;     /* GetFlrcRxStats len_error          */
    unsigned long      next_flrc_stats_jiffies;
    unsigned long      next_fifo_flags_jiffies;  /* IMP-2c: 1 Hz FIFO-flags poll */

    /* Wedge watchdog. Detects "we're in RADIO_MODE_RX but DIO9 hasn't
     * fired in over LR2021_WEDGE_GRACE_MS milliseconds" and attempts
     * recovery. See lr2021_wedge.c.
     *
     * last_dio9_ktime is stamped by lr2021_dio9_thread on every IRQ
     * entry via WRITE_ONCE, read by the watchdog work callback via
     * READ_ONCE. WRITE/READ_ONCE pairing prevents the compiler from
     * tearing the 64-bit ktime_t store/load on 32-bit kernels (harmless
     * on aarch64 where 64-bit loads/stores are single instructions, but
     * matches kernel style for cross-CPU shared scalars).
     *
     * Recovery runs from a delayed_work, not a kernel timer: the soft
     * path takes cmd_mutex and issues SPI commands that sleep, both
     * illegal in softirq (a plain timer_list callback runs in softirq
     * and triggers "BUG: scheduling while atomic"). delayed_work runs
     * in process context where sleeping is fine.
     *
     * wedge_recoveries packs two 16-bit counters: low half = soft
     * (SetStandby+SetRx), high half = hard (NRESET+hw_init). The
     * heartbeat decodes them for display. */
    ktime_t                 last_dio9_ktime;
    atomic_t                wedge_recoveries;
    struct delayed_work     wedge_dwork;

    /* RX chip-liveness probe (lr2021_wedge.c). wedge_probe is the
     * GetFlrcRxStats baseline taken at ~GRACE/2 into a DIO9-silent episode;
     * counters frozen vs it at GRACE (+ chip in Rx + FIFO empty) proves the
     * silence is RF-quiet, not a wedge, and recovery is skipped. Baseline +
     * flags are single-writer (the wedge delayed_work); wedge_quiet_skips
     * counts skipped episodes for debugfs (atomic: lockless reader). */
    struct lr2021_flrc_rx_stats wedge_probe;
    bool                    wedge_probe_valid;
    bool                    wedge_quiet_noted;
    atomic_t                wedge_quiet_skips;
} LR2021_Device;

#include "lr2021_spi.h"   /* SPI command/FIFO/async API */

/* TX workqueue worker (lr2021_tx.c). */
void lr2021_tx_work(struct work_struct *work);

/* submit_locked() result (lr2021_tx.c). Caller holds cmd_mutex. */
enum lr2021_submit_result {
    LR2021_SUBMIT_OK    = 0,  /* a packet was written + SetTx issued     */
    LR2021_SUBMIT_EMPTY = 1,  /* ring empty: tx_inflight cleared         */
    LR2021_SUBMIT_ERR   = 2,  /* SPI error: tx_inflight cleared          */
};

/* Pop one record and submit it (fixed-pad or dynamic pld_len reprogram +
 * fifo_write + SetTx). Caller MUST hold cmd_mutex and MUST NOT hold tx_lock.
 * On EMPTY/ERR it clears tx_inflight under tx_lock internally. Defined in
 * lr2021_tx.c. */
enum lr2021_submit_result lr2021_tx_submit_locked(LR2021_Device *dev);

/* Refill the in-flight packet's TX FIFO (TxFifo-low IRQ). Caller holds
 * cmd_mutex. Defined in lr2021_tx.c. */
void lr2021_tx_refill_locked(LR2021_Device *dev);

/* Apply dev->flrc_cfg to the chip (SetFlrcModulationParams +
 * SetFlrcPacketParams + ConfigFifoIrq). Takes cmd_mutex itself; callers
 * must NOT hold it. Defined in lr2021_core.c. */
int  lr2021_apply_flrc_config(LR2021_Device *dev);

#ifdef FLRC_PROBE
/* Probe: transmit one dynamic-mode FLRC packet of `len` bytes
 * (6..255) via mechanism (a) -- pld_len reprogram + payload-only write.
 * Defined in lr2021_tx.c. */
int lr2021_probe_dyn_tx(LR2021_Device *dev, u16 len);
#endif

/* DIO9 threaded IRQ handler (lr2021_irq.c). */
irqreturn_t lr2021_dio9_thread(int irq, void *data);

/* Bump dev->errors_irq (IRQ-path hard errors) under `lock`, safe from any
 * context. Defined in lr2021_irq.c; also called from the TX refill path. */
void lr2021_bump_irq_error(LR2021_Device *dev);

/* Hardware init / reset (lr2021_init.c). hw_init runs the FLRC bring-up
 * sequence and takes cmd_mutex itself; hw_reset pulses NRESET and is
 * caller-locked (see RADIO_IOCTL_RESET in lr2021_core.c). Both are used
 * by the worker's second-strike recovery path and by the RESET ioctl. */
int  lr2021_hw_init (LR2021_Device *dev);
void lr2021_hw_reset(LR2021_Device *dev);

/* Wedge watchdog (lr2021_wedge.c). Init must be called AFTER probe has
 * installed the threaded IRQ and configured the chip; destroy must be
 * called BEFORE devm_free_irq in remove, so the timer callback can never
 * race with IRQ thread teardown nor see a half-freed device. */
void lr2021_wedge_init   (LR2021_Device *dev);
void lr2021_wedge_destroy(LR2021_Device *dev);

#endif
