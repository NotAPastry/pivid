#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include "lr2021.h"
#include "lr2021_commands.h"
#include "lr2021_ring.h"
#include "lr2021_spi.h"

#ifdef TXPERF
/* <linux/ktime.h> is already included unconditionally above (for ktime_get). */
#include <linux/math64.h>
#define TXPERF_WINDOW 2000u
#endif

/* FLRC payload ceiling matching lr2021_init.c flrc_pkt_params[2:3] =
 * pld_len(15:0) = 0x01FF = 511. The link runs DYNAMIC only: the worker
 * reprograms per-packet pld_len with SetFlrcPacketParams and writes
 * payload-only -- no padding -- so this constant is only the upper bound
 * (mode==0 branch in submit_locked). The fixed path below (mode!=0) is a
 * legacy fallback kept in the encoder but never selected at probe: with
 * pkt_fmt=1 (fixed, datasheet 18.4.2) SetTx ALWAYS pulls exactly pld_len
 * bytes from the TX FIFO, so writing fewer causes TxFifoUnderflow and stale
 * FIFO bytes leak onto the wire -- the RX-side protocol_parse then treats the
 * trailing garbage as a valid packet (random flags + length pass parsing),
 * which the reassembler counts as a SEQ mismatch and the train is dropped. So
 * in fixed mode the worker pads every short packet to LR2021_FLRC_PLD_LEN with
 * deterministic 0x00 bytes below. */

/* RE-PRIME INVARIANT (the riskiest concurrency in this driver):
 *   "TX never goes idle (tx_inflight==false) while the TX ring is non-empty."
 * Enforced by making BOTH the worker's start-the-chain claim and
 * lr2021_tx_submit_locked's empty-ring clear happen under dev->tx_lock, with
 * the empty path re-checking the ring (lr2021_ring_tx_nonempty) under that same
 * lock before clearing -- so a producer that raced a record in between the pop
 * and the clear is observed and the chain continues (no stranded packet). The
 * producer always queue_work()-kicks after bumping wr_idx. Stress-tested in
 * Phase C, saturation TX: 30000 packets with bursty enqueue
 * (producer races the chain end) AND a low-pps trickle (--pps 30) that
 * re-primes from idle on almost every packet. PASS = zero stranded packets
 * (RX recs == TX count minus RF loss), zero WARN/BUG in dmesg, no wedge
 * recovery, throughput ~1.5-1.6 Mbps. */

#define LR2021_FLRC_PLD_LEN 511

/* FLRC minimum payload (DS 18.2.2 / 18.4.2: "allowed range of pld_len is from 6
 * to 511 bytes"). A dynamic-mode SetFlrcPacketParams with pld_len < 6 is
 * rejected by the chip (CMD_PERR), after which SetTx fires with stale params ->
 * wrong-length or dropped packet (BUG-4 / IMP-8). Production userspace never
 * enqueues a sub-6 record -- the smallest is the padded --frame-trailer at 6 B
 * (encoder.c) and OSD fragments at >= 10 B -- so this guard is a defensive net
 * that must not fire in normal operation; if it does, tx_runt_drops surfaces a
 * userspace regression instead of a silent malformed transmission. */
#define LR2021_FLRC_MIN_PLD 6u

/* The TX FIFO is LR2021_FIFO_CHUNK_BYTES (256, defined in lr2021.h); the first
 * FIFO write at submit can fill it entirely, then each TxFifo-low refill tops it
 * up by the freed space. */

/* SetTx tx_timeout argument: 24-bit value in 32 kHz RTC ticks. 0 disables
 * the chip-level timeout entirely -- hang detection is the wedge watchdog
 * (lr2021_wedge.c, Task B8) via tx_inflight + last_submit_ktime. */
static const u8 lr2021_set_tx_args[3] = { 0x00, 0x00, 0x00 };

#ifdef FLRC_PROBE
/* Probe mechanism (a): per-packet length via SetFlrcPacketParams.pld_len.
 * `len` is the dynamic on-air length (1..511, the 9-bit dynamic-length
 * header max). For len<=255 this sends a complete valid packet of `len`
 * incrementing bytes (0,1,2,...). For len>255 it deliberately underflows
 * the TX FIFO: the chip header says `len` but we write only 255 bytes, so
 * RX decodes an out-of-range on-air length and flags LenError (the LenError
 * probe). Returns 0 on SPI success (does NOT verify reception --
 * the RX node does).
 * NOTE: does not wait for TX_DONE and does not restore prior packet params.
 * Invoke single-shot / manually paced (daemons stopped) so a still-BUSY chip
 * from a previous send cannot corrupt the next -- back-to-back calls without
 * pacing can make the mechanism look broken when it is not. */
int lr2021_probe_dyn_tx(LR2021_Device *dev, u16 len)
{
    u8 pkt_params[4];
    u8 payload[255];
    u16 write_len = len > 255 ? 255 : len;
    u16 i;
    int ret;

    if (len < 1 || len > 511)
        return -EINVAL;
    for (i = 0; i < write_len; i++)
        payload[i] = (u8)i;

    /* Dynamic mode: mirror init flrc_pkt_params {0x1E,0x4D,...} but clear
     * pkt_fmt (byte1 bit2) -> 0x49, and set pld_len = len. Preamble/sync/crc
     * unchanged so the RX (also dynamic in the probe build) stays in sync. */
    pkt_params[0] = 0x1E;
    pkt_params[1] = 0x49;
    pkt_params[2] = (u8)(len >> 8);
    pkt_params[3] = (u8)(len & 0xFF);

    mutex_lock(&dev->cmd_mutex);
    /* Start each send from a known-empty FIFO: a prior >255 provocation
     * underflows the TX FIFO (writes <pld_len bytes), which would otherwise
     * corrupt the next valid send and make an RX resync look broken. */
    ret = lr2021_spi_cmd(dev, LR2021_CMD_CLEAR_TX_FIFO, NULL, 0, NULL, 0);
    if (ret == 0)
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_FLRC_PACKET_PARAMS,
                             pkt_params, sizeof(pkt_params), NULL, 0);
    if (ret == 0)
        ret = lr2021_spi_fifo_write(dev, payload, write_len);   /* payload only */
    if (ret == 0)
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_TX,
                             lr2021_set_tx_args, sizeof(lr2021_set_tx_args),
                             NULL, 0);
    /* This send reprogrammed SetFlrcPacketParams out-of-band; invalidate the
     * normal-path length cache so the next lr2021_tx_submit_locked reprograms. */
    dev->tx_len_valid = false;
    mutex_unlock(&dev->cmd_mutex);
    dev_info(&dev->spi->dev, "FLRC_PROBE dyn_tx len=%u write=%u mech=a ret=%d\n", len, write_len, ret);
    return ret;
}
#endif

static void lr2021_count_packet_tx(LR2021_Device *dev, u16 nbytes) {
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, flags);
    dev->packets_tx++;
    dev->bytes_tx += nbytes;   /* real payload, not the pad airtime */
    spin_unlock_irqrestore(&dev->lock, flags);
}

/* Pop one TX record and put it on the wire. Caller holds cmd_mutex.
 *
 * Fixed mode (cfg.mode==1): the chip is configured for cfg.pld_len bytes; pad
 * the popped record to pld_len with zeros and write exactly that many.
 *
 * Dynamic mode (cfg.mode==0): reprogram SetFlrcPacketParams.pld_len to this
 * packet's real length, then write payload ONLY -- the chip generates the
 * 9-bit on-air length header.
 *
 * On empty ring or SPI error, clear tx_inflight under tx_lock and return the
 * matching result so the caller (worker or IRQ) stops the chain. */
#ifdef TXPERF
/* Accumulate per-packet TX timing and emit a windowed average every
 * TXPERF_WINDOW packets. Inputs are captured by the caller before any SPI work:
 * txp_start = submit start; txp_air_ns/txp_gap_ns = the prior packet's air time
 * and inter-packet gap. Debug builds (TXPERF) only. */
static void lr2021_txperf_account(LR2021_Device *dev, ktime_t txp_start,
                                  s64 txp_air_ns, s64 txp_gap_ns) {
    static ktime_t txp_prev_start;
    static s64     acc_submit_ns, acc_air_ns, acc_gap_ns, acc_period_ns;
    static u32     acc_n;
    s64 submit_ns = ktime_to_ns(ktime_sub(ktime_get(), txp_start));

    acc_submit_ns += submit_ns;
    acc_air_ns    += txp_air_ns;
    acc_gap_ns    += txp_gap_ns;
    if (ktime_to_ns(txp_prev_start) != 0)
        acc_period_ns += ktime_to_ns(ktime_sub(txp_start, txp_prev_start));
    txp_prev_start = txp_start;
    if (++acc_n >= TXPERF_WINDOW) {
        s64 n = acc_n;
        dev_info(&dev->spi->dev,
            "TXPERF %u pkts avg(us): submit=%lld air=%lld gap=%lld period=%lld\n",
            acc_n,
            div_s64(acc_submit_ns, n) / 1000,
            div_s64(acc_air_ns, n) / 1000,
            div_s64(acc_gap_ns, n) / 1000,
            div_s64(acc_period_ns, n) / 1000);
        acc_submit_ns = acc_air_ns = acc_gap_ns = acc_period_ns = 0;
        acc_n = 0;
    }
}
#endif

enum lr2021_submit_result lr2021_tx_submit_locked(LR2021_Device *dev) {
    unsigned long flags;
    u16 len = 0;        /* real payload bytes of the popped record           */
    u16 total = 0;      /* full on-wire bytes (dynamic: len; fixed: plen)    */
    int pop_ret;
    int ret;
#ifdef TXPERF
    /* Capture submit-start and the air/gap inputs BEFORE any SPI work and
     * BEFORE last_submit_ktime is overwritten below: txp_last must hold the
     * PREVIOUS packet's submit time so air = its submit -> its TX_DONE. The
     * windowed accumulation happens in lr2021_txperf_account() after submit. */
    ktime_t txp_start = ktime_get();
    ktime_t txp_done  = READ_ONCE(dev->tx_done_ktime);
    ktime_t txp_last  = READ_ONCE(dev->last_submit_ktime);
    s64 txp_gap_ns = (ktime_to_ns(txp_done) != 0)
                   ? ktime_to_ns(ktime_sub(txp_start, txp_done)) : 0;
    s64 txp_air_ns = (ktime_to_ns(txp_done) != 0 && ktime_to_ns(txp_last) != 0)
                   ? ktime_to_ns(ktime_sub(txp_done, txp_last)) : 0;
#endif

    /* Pop with an empty-ring re-check under tx_lock to close the end-of-burst
     * stranded-packet race: a producer that published wr_idx + queued the
     * worker in the window between our pop (saw empty) and the clear-to-false
     * would otherwise be lost (the worker ran, saw inflight still true, backed
     * off). By re-peeking the ring under the SAME tx_lock section as the clear,
     * EITHER we see the new record here (keep inflight, retry the pop) OR the
     * producer's wr_idx publish lands after this peek and its queue_work finds
     * inflight==false -> the worker re-primes. Same acquire-load of wr_idx in
     * pop and lr2021_ring_tx_nonempty makes the two coherent. */
    for (;;) {
        pop_ret = lr2021_ring_tx_pop(dev, dev->tx_refill_buf, &len);
        /* IMP-8 (BUG-4): enforce the FLRC 6-byte minimum before the chip sees
         * the record. In dynamic mode a sub-6 pld_len wedges
         * SetFlrcPacketParams (CMD_PERR), then SetTx fires with stale params ->
         * wrong-length or dropped packet (see the LR2021_FLRC_MIN_PLD note).
         * Drop the runt and pull the next record IN THIS loop -- reusing the
         * empty-ring re-check below keeps the re-prime invariant intact. Drop,
         * not pad: a driver pad would desync the RX in-band framer (the outer
         * LE16 set by userspace would no longer match the on-air length).
         * Production never hits this (smallest record is the padded 6-byte
         * --frame-trailer); a nonzero tx_runt_drops flags a userspace
         * regression. Fixed mode (mode!=0) zero-pads to pld_len below, so the
         * guard is dynamic-only. */
        if (pop_ret == 1 && len < LR2021_FLRC_MIN_PLD && dev->flrc_cfg.mode == 0) {
            dev_warn_ratelimited(&dev->spi->dev,
                                 "TX runt %u < %u B min, dropping\n",
                                 len, LR2021_FLRC_MIN_PLD);
            spin_lock_irqsave(&dev->lock, flags);
            dev->tx_runt_drops++;
            spin_unlock_irqrestore(&dev->lock, flags);
            continue;   /* re-pop; may now see empty and re-prime cleanly */
        }
        if (pop_ret == 0) {
            spin_lock_irqsave(&dev->tx_lock, flags);
            /* RE-PRIME INVARIANT: "TX never goes idle (tx_inflight==false)
             * while the TX ring is non-empty." Enforced HERE: we only clear
             * inflight (below) after observing the ring empty under tx_lock; if
             * a producer raced a record in, we see it here and stay in the
             * chain (retry) -- so no packet is stranded at end-of-burst. */
            if (lr2021_ring_tx_nonempty(dev)) {
                /* Producer published between our pop and here: stay in the
                 * chain (inflight stays true) and retry the pop. */
                spin_unlock_irqrestore(&dev->tx_lock, flags);
                continue;
            }
            dev->tx_inflight = false;
            WRITE_ONCE(dev->last_submit_ktime, 0);
            spin_unlock_irqrestore(&dev->tx_lock, flags);
            return LR2021_SUBMIT_EMPTY;
        }
        break;   /* pop_ret == 1 (record) or pop_ret < 0 (corrupt) */
    }

    if (pop_ret < 0) {
        dev_err_ratelimited(&dev->spi->dev,
                            "TX ring corrupt (pop returned %d)\n", pop_ret);
        spin_lock_irqsave(&dev->tx_lock, flags);
        dev->tx_inflight = false;
        WRITE_ONCE(dev->last_submit_ktime, 0);
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return LR2021_SUBMIT_ERR;
    }

    if (dev->flrc_cfg.mode == 0) {
        /* Dynamic: set the on-air length to this packet's real length. The
         * SetFlrcPacketParams reprogram must precede the FIFO write. Skip the
         * SPI round-trip when the length already matches what we last programmed
         * (the FEC packer emits mostly identical-length pairs) -- only pld_len
         * varies across packets, so a length match means the chip params are
         * already correct. cached_tx_len is set ONLY after a successful program,
         * so a failed SPI cmd cannot poison the cache. */
        ret = 0;
        if (!dev->tx_len_valid || dev->cached_tx_len != len) {
            u8 pkt_params[4];
            struct radio_flrc_config tmp = dev->flrc_cfg;
            tmp.pld_len = len;
            lr2021_flrc_encode_params(&tmp, pkt_params);
            ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_FLRC_PACKET_PARAMS,
                                 pkt_params, sizeof(pkt_params), NULL, 0);
            if (ret == 0) {
                dev->cached_tx_len = len;
                dev->tx_len_valid  = true;
            }
        }
        total = len;   /* payload-only; the chip generates the on-air header */
    } else {
        /* Fixed: pad to pld_len. Clamp defensively so a malformed record can
         * never memset out of bounds. */
        u16 plen = dev->flrc_cfg.pld_len;
        if (plen > LR2021_FLRC_PLD_LEN)
            plen = LR2021_FLRC_PLD_LEN;
        if (len > plen)
            len = plen;
        if (len < plen)
            memset(&dev->tx_refill_buf[len], 0, (size_t)plen - len);
        total = plen;   /* on-wire = padded pld_len; len stays real payload */
        ret = 0;
    }

    /* Write the FIRST chunk into the 256B TX FIFO and arm the refill cursor.
     * For packets <= 256, done == total here and the TxFifo-low IRQ no-ops.
     * For larger packets, lr2021_tx_refill_locked tops up the rest. `total` is
     * the full on-wire byte count (dynamic payload-only / fixed padded
     * pld_len); `len` stays the real payload for the throughput counter. */
    if (ret == 0) {
        u16 first = total > LR2021_FIFO_CHUNK_BYTES ? LR2021_FIFO_CHUNK_BYTES : total;
        ret = lr2021_spi_fifo_write(dev, dev->tx_refill_buf, first);
        if (ret == 0) {
            dev->tx_refill_total = total;
            dev->tx_refill_done  = first;
        }
    }

    if (ret == 0)
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_TX,
                             lr2021_set_tx_args, sizeof(lr2021_set_tx_args),
                             NULL, 0);

    if (ret) {
        dev_warn_ratelimited(&dev->spi->dev, "TX submit SPI failed: %d\n", ret);
        spin_lock_irqsave(&dev->tx_lock, flags);
        dev->tx_inflight = false;
        WRITE_ONCE(dev->last_submit_ktime, 0);
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return LR2021_SUBMIT_ERR;
    }

    WRITE_ONCE(dev->last_submit_ktime, ktime_get());
    lr2021_count_packet_tx(dev, len);   /* real payload bytes, not the pad */
#ifdef TXPERF
    lr2021_txperf_account(dev, txp_start, txp_air_ns, txp_gap_ns);
#endif
    return LR2021_SUBMIT_OK;
}

/* Refill the TX FIFO for the in-flight >256 B packet. Caller holds cmd_mutex.
 * Reads GetTxFifoLevel to size the write to the freed space, writes the next
 * chunk from tx_refill_buf[tx_refill_done..], advances the cursor. No-op if the
 * current packet is fully written (done == total). The chip keeps draining the
 * FIFO at the info rate while we write -- this overlaps airtime. */
void lr2021_tx_refill_locked(LR2021_Device *dev) {
    u8  lvl_buf[2];
    u16 level, freespace, remain, chunk;
    int ret;

    /* tx_refill_total/_done are valid only while tx_inflight; between packets
     * they are left at done==total. Guard with >= (not the subtraction == 0) so
     * a transient done>total can never wrap the u16 `remain` to a huge value
     * and over-read tx_refill_buf -- structurally impossible rather than relying
     * on cmd_mutex serialisation. Benign no-op until submit_locked re-arms the
     * cursor for the next packet. */
    if (dev->tx_refill_done >= dev->tx_refill_total)
        return;                       /* nothing left for this packet */
    remain = dev->tx_refill_total - dev->tx_refill_done;

    /* GetTxFifoLevel (opcode 0x011D): the 2-byte response after the status
     * bytes is Level(15:8) then Level(7:0) -- a 16-bit FIFO occupancy MSB-
     * first (datasheet table 6-59, verified). lr2021_spi_cmd strips the 2
     * status bytes, so lvl_buf[0]=Level(15:8), lvl_buf[1]=Level(7:0). Free
     * space = FIFO size - level. */
    ret = lr2021_spi_cmd(dev, LR2021_CMD_GET_TX_FIFO_LEVEL,
                         NULL, 0, lvl_buf, sizeof(lvl_buf));
    if (ret) {
        dev_warn_ratelimited(&dev->spi->dev,
                             "TX refill: GetTxFifoLevel failed: %d\n", ret);
        /* IRQ-path failure (refill runs in the TxFifo-low IRQ): count it in
         * errors_irq to match the IRQ RX path. lr2021_spi_cmd() has already
         * bumped errors_spi for the transport-level fault. */
        lr2021_bump_irq_error(dev);
        return;
    }
    level = ((u16)lvl_buf[0] << 8) | (u16)lvl_buf[1];
    if (level >= LR2021_FIFO_CHUNK_BYTES)
        return;                       /* full; wait for next low IRQ */
    freespace = LR2021_FIFO_CHUNK_BYTES - level;

    chunk = remain < freespace ? remain : freespace;
    ret = lr2021_spi_fifo_write(dev, dev->tx_refill_buf + dev->tx_refill_done,
                                chunk);
    if (ret) {
        dev_warn_ratelimited(&dev->spi->dev,
                             "TX refill: fifo_write failed: %d\n", ret);
        /* errors_spi already bumped by lr2021_spi_fifo_write; mirror it in
         * errors_irq like the GetTxFifoLevel path above. */
        lr2021_bump_irq_error(dev);
        return;                       /* underflow likely; wedge wd recovers */
    }
    dev->tx_refill_done += chunk;
}

/* Prime-from-idle. Queued by RADIO_IOCTL_TX_ENQUEUE after userspace bumps
 * wr_idx. If a chain is already in flight the IRQ owns TX, so we back off --
 * the IRQ's TX_DONE submit_locked picks up the new record. Otherwise we claim
 * the chain (set tx_inflight under tx_lock) and submit the first packet under
 * cmd_mutex. The claim is atomic under tx_lock so worker and IRQ never both
 * think they own "start the chain" (the re-prime invariant). If the ring turns
 * out empty after we claim, submit_locked clears tx_inflight again. */
void lr2021_tx_work(struct work_struct *work) {
    LR2021_Device *dev = container_of(work, LR2021_Device, tx_work);
    unsigned long flags;
    bool claim;

    spin_lock_irqsave(&dev->tx_lock, flags);
    if (dev->tx_inflight) {
        claim = false;                 /* IRQ chain owns TX; back off. */
    } else {
        dev->tx_inflight = true;       /* claim the chain. */
        /* last_submit_ktime is NOT stamped until the first successful SetTx
         * below, so the B8 wedge watchdog must guard ktime==0 across this
         * claim->first-submit window (else it sees inflight && ktime==0). */
        claim = true;
    }
    spin_unlock_irqrestore(&dev->tx_lock, flags);

    if (!claim)
        return;

    mutex_lock(&dev->cmd_mutex);
    (void)lr2021_tx_submit_locked(dev);   /* clears tx_inflight if empty/err */
    mutex_unlock(&dev->cmd_mutex);
}
