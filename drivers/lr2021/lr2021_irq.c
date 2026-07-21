#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include "lr2021.h"
#include "lr2021_commands.h"
#include "lr2021_ring.h"
#include "lr2021_rx_walk.h"
#include "lr2021_spi.h"

#ifdef COALESCE_PROBE
/* Multi-packet-per-IRQ probe (RCA 2026-07-05 follow-up). When a second packet
 * finishes (or lands) while the host is still servicing the previous RX_DONE,
 * the single-bit RX_DONE coalesces and GetRxPktLength -- which per DS 7.3.10
 * returns the length of the LAST received payload -- no longer describes the
 * packet at the FIFO read pointer. The probe samples GetRxFifoLevel right
 * after GetRxPktLength at every RX_DONE and logs any packet whose FIFO holds
 * MORE bytes than this packet still needs (surplus >= 6, the FLRC minimum
 * payload, to reject SPI-window trickle of a next packet mid-flight).
 * True coalesce signature: pkt_len == the runt's length (~10) while the level
 * is a whole tail-slice bigger. Single-owner IRQ thread => plain statics. */
static atomic_t coalprobe_rxdone  = ATOMIC_INIT(0);
static atomic_t coalprobe_surplus = ATOMIC_INIT(0);   /* any >=6 B surplus   */
static atomic_t coalprobe_mislen  = ATOMIC_INIT(0);   /* smoking gun (below) */
static u32 coalprobe_irq_status;
#endif

/* Shared with the TX refill path (lr2021_tx.c); declared in lr2021.h. */
void lr2021_bump_irq_error(LR2021_Device *dev) {
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, flags);
    dev->errors_irq++;
    spin_unlock_irqrestore(&dev->lock, flags);
}

static void lr2021_bump_packet_rx(LR2021_Device *dev, u16 nbytes) {
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, flags);
    dev->packets_rx++;
    dev->bytes_rx += nbytes;
    spin_unlock_irqrestore(&dev->lock, flags);
}

static void lr2021_bump_ring_full(LR2021_Device *dev) {
    unsigned long flags;
    spin_lock_irqsave(&dev->lock, flags);
    dev->rx_ring_full_drops++;
    spin_unlock_irqrestore(&dev->lock, flags);
}

/* Determine where the real protocol frame begins in the FIFO-read buffer.
 *
 * The direct-read FIFO frame can carry a stray leading byte before the real
 * payload (a 16 MHz SPI timing artifact; lr2021_spi_fifo_read's post-opcode
 * delay removes it at the source). We read pkt_len+1 bytes and pick the
 * offset (0 = no stray, 1 = stray) PER PACKET -- the stray count varies
 * packet-to-packet, so a single per-load lock misframes the minority that
 * differ (that was project_rx_misframe_fec_fabrication: a clean link with FEC
 * off still fabricated ~7% misframed packets, which the always-on RX FEC
 * parser turned into bogus 100%-unrecoverable blocks).
 *
 * The decision logic + protocol header invariant live in lr2021_frame_sync.h
 * (pure, host-tested in tests/test_frame_sync.c). `p` must have >= 3 readable
 * bytes. Called only from the DIO9 IRQ thread (single owner), so rx_sync needs
 * no lock. Returns 0 or 1.
 *
 * Also tallies the decision class into the debugfs diagnostic counters so the
 * 16 MHz SPI stray-byte rate is measurable directly at its source (and
 * RF-independently -- CRC-failed packets never reach here). */
static int lr2021_rx_frame_offset(LR2021_Device *dev, const u8 *p, u16 pkt_len) {
    enum lr2021_frame_class cls;
    int off;

    /* STRONG disambiguation for the 3-byte-header protocol format (production
     * video, common/protocol.c FIXED mode): the LE16 length field at
     * [off+1..off+2] must equal the payload length pkt_len-3. This is a 16-bit
     * exact match against the chip-reported packet length, so it picks the true
     * offset PER PACKET even when the 1-byte heuristic is ambiguous (measured
     * ~99.6% of packets). That ambiguity was the RX stall: a SPI stray present
     * right after reload then absent left the heuristic's STICKY dominant vote
     * locked to the wrong offset, so every packet misframed and 100% failed
     * protocol_parse downstream (fec_blk/nals froze while packets_rx climbed).
     * The caller reads pkt_len bytes, so for pkt_len>=4 indices 0..3 are valid
     * and the v1 read of p[2..3] is in bounds. Falls back to the per-packet
     * self-sync heuristic for the 1-byte-length linktest format and for genuine
     * non-protocol records (where this exact match holds at neither offset). */
    if (pkt_len >= 4) {
        u16 want = (u16)(pkt_len - 3);
        int v0 = ((u16)(p[1] | ((u16)p[2] << 8)) == want);
        int v1 = ((u16)(p[2] | ((u16)p[3] << 8)) == want);
        if (v0 && !v1) { atomic_inc(&dev->rx_frame_total); return 0; }
        if (v1 && !v0) { atomic_inc(&dev->rx_frame_total);
                         atomic_inc(&dev->rx_frame_stray); return 1; }
    }

    /* DYNAMIC pivid record (OSD-grid keyframe fragment; or the retired value
     * telemetry record) disambiguation by its flags+version signature -- the
     * LE16 check above can't frame it (no LE16 length) and the 1-byte heuristic
     * below rejects it (the 0xC1 TYPE bit trips reserved-bit-0), so without this
     * 100% of these packets misframe onto the video-learned sticky offset
     * (HW-observed: osd_frags_rx stuck at 0). See lr2021_frame_sync.h. */
    {
        int toff;
        if (lr2021_dyn_frame_offset(p, pkt_len, &toff)) {
            atomic_inc(&dev->rx_frame_total);
            if (toff == 1) atomic_inc(&dev->rx_frame_stray);
            return toff;
        }
    }

    off = lr2021_frame_sync_offset_classified(&dev->rx_sync, p, &cls);
    atomic_inc(&dev->rx_frame_total);
    if (cls == LR2021_FC_OFF1)       atomic_inc(&dev->rx_frame_stray);
    else if (cls == LR2021_FC_AMBIG) atomic_inc(&dev->rx_frame_ambig);
    return off;
}

/* Drain whatever the RX FIFO currently holds into the per-packet accumulator
 * (dev->rx_drain_buf at dev->rx_drain_off), WHILE the packet is still being
 * received. Called from the DIO9 thread when the RxFifo-high IRQ fires: a
 * >256 B FLRC payload overflows the 256 B RX FIFO unless we read bytes out
 * during reception (datasheet §5.3.1), so we pull the available bytes here and
 * lr2021_drain_rx_packet reads only the remainder at RX_DONE. Caller must hold
 * dev->cmd_mutex; rx_drain_off is single-owner (this thread) so no extra lock.
 *
 * These are RAW FIFO bytes including any leading stray -- since the very first
 * drain of a packet starts at offset 0, the stray (if present) lands at
 * rx_drain_buf[0], exactly where lr2021_rx_frame_offset expects it. We never
 * write past LR2021_RX_PACKET_BUF: the read is clamped to the remaining buffer
 * space, so a runaway level (or a stray RxFifo-high with no packet in flight)
 * can never overflow rx_drain_buf. */
static void lr2021_drain_rx_during_rx(LR2021_Device *dev) {
    u8  lvl_buf[2];
    u16 level;
    size_t space, chunk;
    int ret;

    /* GetRxFifoLevel (opcode 0x011C): the 2-byte response after the status
     * bytes is Level(15:8) then Level(7:0) -- a 16-bit FIFO occupancy MSB-first,
     * exactly like GetTxFifoLevel in lr2021_tx_refill_locked. lr2021_spi_cmd
     * strips the 2 status bytes, so lvl_buf[0]=Level(15:8), lvl_buf[1]=(7:0). */
    ret = lr2021_spi_cmd(dev, LR2021_CMD_GET_RX_FIFO_LEVEL,
                         NULL, 0, lvl_buf, sizeof(lvl_buf));
    if (ret) {
        dev_warn_ratelimited(&dev->spi->dev,
                             "RX drain: GetRxFifoLevel failed: %d\n", ret);
        lr2021_bump_irq_error(dev);
        return;
    }
    level = ((u16)lvl_buf[0] << 8) | (u16)lvl_buf[1];
    if (level == 0)
        return;                       /* nothing to drain (stray/early IRQ) */

    /* Bound the read to the free space left in the accumulator. rx_drain_off
     * can in principle already be at the buffer end (a stray RxFifo-high after
     * a full packet drained but before RX_DONE reset the cursor) -- in that
     * case space==0 and we read nothing, leaving the chip to LenError/overflow
     * the bogus surplus, which is the correct failure mode. */
    if (dev->rx_drain_off >= LR2021_RX_PACKET_BUF)
        return;
    /* The RX FIFO is 256 B, so level <= 256 always: a single
     * min(level, space, FIFO_CHUNK) read fully drains whatever the IRQ
     * signalled -- no read-loop needed, exactly the single-shot-per-IRQ
     * reasoning the TX refill relies on. */
    space = (size_t)LR2021_RX_PACKET_BUF - dev->rx_drain_off;
    chunk = level < space ? level : space;
    if (chunk > LR2021_FIFO_CHUNK_BYTES)
        chunk = LR2021_FIFO_CHUNK_BYTES;   /* one FIFO frame per read */

    ret = lr2021_spi_fifo_read(dev, dev->rx_drain_buf + dev->rx_drain_off,
                               chunk);
    if (ret) {
        dev_warn_ratelimited(&dev->spi->dev,
                             "RX drain: FIFO read failed: %d\n", ret);
        lr2021_bump_irq_error(dev);
        return;
    }
    dev->rx_drain_off += (u16)chunk;
}

/* Legacy single-packet emit: byte-identical to the pre-walker path, used when
 * the stream head is not in-band framable (DYN/OSD record, linktest 1-byte
 * format, garbage). Decides the frame offset via the pkt_len exact-match /
 * DYN-signature / sticky-heuristic chain, tops up the stray tail byte only
 * when the accumulator holds exactly pkt_len bytes (in the stray case the
 * FIFO then holds precisely one more real byte, so the read is level-safe),
 * and pushes pkt_len bytes at the offset. Returns 1 if a packet was pushed.
 * Caller must hold dev->cmd_mutex. */
static int lr2021_rx_emit_legacy(LR2021_Device *dev, u16 pkt_len, u16 acc) {
    u8 *payload = dev->rx_drain_buf;
    int offset = lr2021_rx_frame_offset(dev, payload, pkt_len);
    int ret;

    if (offset == 1 && acc == pkt_len) {
        ret = lr2021_spi_fifo_read(dev, payload + pkt_len, 1);
        if (ret) {
            dev_warn_ratelimited(&dev->spi->dev,
                                 "RX FIFO stray-tail read failed: %d\n", ret);
            lr2021_bump_irq_error(dev);
            return 0;
        }
    }
    ret = lr2021_ring_rx_push(dev, payload + offset, pkt_len);
    if (ret) {
        /* Userspace daemon is too slow draining the kernel ring -- packet is
         * lost. Counted separately from errors_irq so "host overrun" is
         * distinguishable from RF/IRQ-path errors. */
        dev_warn_ratelimited(&dev->spi->dev,
                             "RX ring full, dropping %u byte packet\n",
                             pkt_len);
        lr2021_bump_ring_full(dev);
        return 0;
    }
    lr2021_bump_packet_rx(dev, pkt_len);
    return 1;
}

/* Pull one received packet out of the chip's Rx FIFO and push it into the
 * mmap'd kernel-side rx_ring. Called from the threaded IRQ when the
 * RX_DONE bit is set. Caller must hold dev->cmd_mutex.
 *
 * crc_failed: when true (CRC_ERROR was set in this IRQ alongside RX_DONE)
 * we still drain the FIFO bytes (the chip requires it to advance the read
 * pointer per datasheet §5.3.1: "all received data is written to the data
 * buffer, even if the CRC is invalid"), but we DROP the packet rather than
 * forwarding it to userspace -- its bytes are random and would otherwise
 * pollute the SEQ-driven reassembler. */
static void lr2021_drain_rx_packet(LR2021_Device *dev, bool crc_failed) {
    u8  len_buf[2];
    u8 *payload = dev->rx_drain_buf;
    u16 pkt_len;
    int ret;

    /* Datasheet table 7-14: response is pkt_len(15:8) then pkt_len(7:0)
     * after the 2 status bytes. Must read both, not just the MSB --
     * otherwise every packet under 256 bytes looks like length 0. */
    ret = lr2021_spi_cmd(dev, LR2021_CMD_GET_RX_PACKET_LENGTH,
                         NULL, 0, len_buf, sizeof(len_buf));
    if (ret) {
        dev_warn_ratelimited(&dev->spi->dev,
                             "RX get-length failed: %d\n", ret);
        lr2021_bump_irq_error(dev);
        dev->rx_drain_off = 0;   /* abandon this packet's accumulation */
        return;
    }

    pkt_len = ((u16)len_buf[0] << 8) | (u16)len_buf[1];

    /* Lower bound 2: any valid protocol packet has at least flags+length.
     * Upper bound LR2021_RX_PACKET_BUF-1: on a stray we write the follow-up
     * tail byte to payload[pkt_len] below, so pkt_len must leave room for that
     * extra slot. NOTE: payload now aliases dev->rx_drain_buf (a u8*), so
     * a sizeof on it would yield the pointer size (8), not the buffer size --
     * use the LR2021_RX_PACKET_BUF macro explicitly. The FLRC handler caps at
     * 511 (LR2021_FLRC_PLD_LEN). */
    if (pkt_len < 2 || pkt_len > LR2021_RX_PACKET_BUF - 1) {
        dev_warn_ratelimited(&dev->spi->dev,
                             "RX bogus length %u\n", pkt_len);
        lr2021_bump_irq_error(dev);
        dev->rx_drain_off = 0;   /* abandon this packet's accumulation */
        return;
    }

#ifdef COALESCE_PROBE
    /* Sample the FIFO level at the moment production trusts pkt_len. remain =
     * bytes this packet still needs (pkt_len minus the during-Rx drain's take).
     * Any level beyond remain is another packet's bytes already appended in
     * RX-continuous mode; >= 6 B of surplus cannot be mid-flight trickle of a
     * barely-started successor alone -- with the ~10 B trailer on air it means
     * the runt is fully queued behind (or ahead of!) this packet. */
    {
        u8  bl[2];
        int bret = lr2021_spi_cmd(dev, LR2021_CMD_GET_RX_FIFO_LEVEL,
                                  NULL, 0, bl, sizeof(bl));
        atomic_inc(&coalprobe_rxdone);
        if (!bret) {
            u16 lvl    = ((u16)bl[0] << 8) | (u16)bl[1];
            u16 remain = (pkt_len > dev->rx_drain_off)
                       ? (u16)(pkt_len - dev->rx_drain_off) : 0;
            /* Benign class (counted only): next packet's bytes trickling in
             * behind a fully/partially drained current packet -- normal
             * RX-continuous pipelining, seen ~constantly at 450 pkt/s. */
            if (lvl > remain && (u16)(lvl - remain) >= 6)
                atomic_inc(&coalprobe_surplus);
            /* Smoking gun (counted + logged): the reported length cannot
             * describe the packet at the FIFO read pointer -- either the
             * during-Rx drain already pulled MORE bytes than pkt_len claims
             * (length belongs to a later, smaller packet: the coalesced-
             * RX_DONE / GetRxPktLength-is-LAST hazard), or a runt-sized
             * length (trailer is ~10 B; video is >200 B) arrives with a
             * video-packet-sized FIFO behind it. */
            if (dev->rx_drain_off > pkt_len ||
                (pkt_len <= 16 && lvl > remain && (u16)(lvl - remain) >= 32)) {
                atomic_inc(&coalprobe_mislen);
                dev_info(&dev->spi->dev,
                         "COALPROBE MISLEN irq=0x%08x pkt_len=%u drained=%u lvl=%u crc=%d\n",
                         coalprobe_irq_status, pkt_len, dev->rx_drain_off,
                         lvl, (int)crc_failed);
            }
        }
    }
#endif

    /* Fast fill: read the packet body up to pkt_len in <=256 B chunks
     * (exact-read discipline -- the BUG-2 fix; the read pointer persists
     * across back-to-back ReadRadioRxFifo frames). For a >256 B packet the
     * RxFifo-high IRQ already drained the first `dev->rx_drain_off` raw bytes
     * during reception; read only the remainder. If the accumulator already
     * holds MORE than pkt_len (during-Rx over-drain across a packet boundary,
     * or a coalesced RX_DONE where pkt_len describes a LATER packet -- see
     * lr2021_rx_walk.h), read nothing here: the walker below frames the head
     * packet by its own in-band header.
     *
     * A stray that lands on a chunk boundary on a >256 B read is a Phase-C
     * tester item (not yet handled here). */
    {
        size_t want = (size_t)pkt_len;
        size_t off  = dev->rx_drain_off;

        while (off < want) {
            size_t chunk = want - off;
            if (chunk > LR2021_FIFO_CHUNK_BYTES)
                chunk = LR2021_FIFO_CHUNK_BYTES;
            ret = lr2021_spi_fifo_read(dev, payload + off, chunk);
            if (ret) {
                dev_warn_ratelimited(&dev->spi->dev,
                                     "RX FIFO read failed: %d\n", ret);
                lr2021_bump_irq_error(dev);
                dev->rx_drain_off = 0;   /* abandon this packet */
                return;
            }
            off += chunk;
        }
        dev->rx_drain_off = (u16)off;

#ifdef FLRC_PROBE
        dev_info(&dev->spi->dev,
                 "FLRC_PROBE rx pkt_len=%u crc_failed=%d first=%*ph\n",
                 pkt_len, (int)crc_failed,
                 (int)min_t(u16, pkt_len, (u16)8), payload);
#endif

        if (crc_failed) {
            /* Corrupt bytes -- count as a CRC drop (RF), not an IRQ error, and
             * do NOT run frame-sync (or the walker) on garbage. We drained
             * exactly pkt_len; a rare stray+CRC-fail leaves one real byte in
             * the FIFO, which the next good packet's per-packet frame-sync
             * detects and corrects. On a coalesced good+bad batch the good
             * head's bytes are dropped too -- the coalesced status word cannot
             * say WHICH packet failed; this matches the old behavior and is
             * ~0.03/s * the coalesce rate. */
            atomic_inc(&dev->crc_drops);
            dev->rx_drain_off = 0;
            return;
        }
    }

    /* Walk the accumulated stream and emit every COMPLETED packet it holds.
     *
     * RX_DONE coalesces (single status bit) and GetRxPktLength describes only
     * the LAST received packet (DS 7.3.10), so when a second packet finished
     * during servicing, pkt_len does NOT describe the FIFO head. The walker
     * (lr2021_rx_walk.h, host-tested) frames each head packet by the pkt_len
     * exact-match test first and its own in-band flags+LE16 header otherwise.
     * HW-measured before this fix: the head packet was misframed at 0.34/s in
     * plain production traffic (small OSD/telemetry behind video) and 1.19/s
     * with the ~5 B frame trailer on air -- ~1:1 with damaged AUs.
     *
     * The clean single-packet case exits after one EMIT with ZERO added SPI
     * traffic (same cost as the old path). Only anomalies go slow: each NEED
     * is level-gated so we read exactly the bytes proven to be in the FIFO; a
     * NEED beyond the level means the head is the packet still in flight --
     * keep the residue accumulated (rx_drain_off) and let its own RX_DONE
     * finish it. An equal-length coalesce is indistinguishable here (fast exit
     * with the successor stranded in the FIFO); it is recovered one service
     * later when the next RX_DONE finds acc > pkt_len. */
    {
        u16  acc     = dev->rx_drain_off;
        bool slow    = false;
        int  pushed  = 0;
        int  emitted = 0;
        int  iter;

        for (iter = 0; iter < 32; iter++) {
            int off = 0;
            u16 n   = 0;
            int act = lr2021_rx_walk_step(payload, acc, pkt_len, &off, &n);

            if (act == LR2021_WALK_EMIT) {
                u16 consume = (u16)((u16)off + n);

                ret = lr2021_ring_rx_push(dev, payload + off, n);
                if (ret) {
                    dev_warn_ratelimited(&dev->spi->dev,
                                         "RX ring full, dropping %u byte packet\n",
                                         n);
                    lr2021_bump_ring_full(dev);
                } else {
                    lr2021_bump_packet_rx(dev, n);
                    pushed++;
                    if (emitted > 0)
                        atomic_inc(&dev->rx_walk_extra);
                }
                /* Diagnostic parity with the old LE16 path: count the framing
                 * decision (no sticky-heuristic vote -- exact/in-band framings
                 * never voted before either). */
                atomic_inc(&dev->rx_frame_total);
                if (off == 1)
                    atomic_inc(&dev->rx_frame_stray);
                emitted++;
                acc = (u16)(acc - consume);
                memmove(payload, payload + consume, acc);
                if (acc == 0 && !slow)
                    break;   /* clean fast path: no evidence of more */
                continue;
            }

            if (act == LR2021_WALK_NEED) {
                u8  lb[2];
                u16 level, got;

                slow = true;
                ret = lr2021_spi_cmd(dev, LR2021_CMD_GET_RX_FIFO_LEVEL,
                                     NULL, 0, lb, sizeof(lb));
                if (ret) {
                    dev_warn_ratelimited(&dev->spi->dev,
                                         "RX walk: GetRxFifoLevel failed: %d\n",
                                         ret);
                    lr2021_bump_irq_error(dev);
                    break;
                }
                level = ((u16)lb[0] << 8) | (u16)lb[1];
                if (level < n)
                    break;   /* head still in flight (or stream end) */
                if ((size_t)acc + n > LR2021_RX_PACKET_BUF) {
                    /* Unreachable by construction (acc + NEED == head extent
                     * <= 512); defensive so a walker bug can't overflow. */
                    atomic_inc(&dev->rx_walk_drops);
                    acc = 0;
                    break;
                }
                got = 0;
                while (got < n) {
                    size_t chunk = (size_t)(n - got);
                    if (chunk > LR2021_FIFO_CHUNK_BYTES)
                        chunk = LR2021_FIFO_CHUNK_BYTES;
                    ret = lr2021_spi_fifo_read(dev, payload + acc + got, chunk);
                    if (ret)
                        break;
                    got = (u16)(got + chunk);
                }
                if (got < n) {
                    dev_warn_ratelimited(&dev->spi->dev,
                                         "RX walk: FIFO read failed: %d\n", ret);
                    lr2021_bump_irq_error(dev);
                    acc = 0;   /* stream integrity gone; abandon */
                    break;
                }
                acc = (u16)(acc + n);
                continue;
            }

            /* LR2021_WALK_LEGACY: head not in-band framable (a DYN/OSD record,
             * linktest 1-byte format, or garbage). Only pkt_len can size it,
             * and pkt_len describes the LAST received packet -- so it is
             * trustworthy ONLY when this head IS that packet, i.e. acc ==
             * pkt_len. That covers the whole non-coalesced world (a lone OSD
             * record) AND the common coalesced order [video..][OSD] where the
             * in-band video packets are peeled off first and the trailing OSD
             * record lands here as the sole remainder (acc == pkt_len). Take
             * the byte-identical old single-packet path there.
             *
             * acc != pkt_len means a DYN record is buried at the HEAD of a
             * coalesced batch (rare: OSD ~5/s must win the coalesce race): its
             * length is unknowable (no in-band header) so we cannot frame it or
             * anything behind it. Drop the residue rather than push a misframed
             * packet -- strictly better than the old pkt_len-at-the-wrong-head
             * corruption, and bounded by the OSD-coalesce rate. */
            if (acc == pkt_len)
                pushed += lr2021_rx_emit_legacy(dev, pkt_len, acc);
            else
                atomic_inc(&dev->rx_walk_drops);
            acc = 0;
            break;
        }

        dev->rx_drain_off = acc;   /* preserve any in-flight residue */
        if (pushed)
            wake_up_interruptible(&dev->rx_waitq);
    }
}

/* Chip-side diagnostics, read under cmd_mutex (caller must hold it).
 *
 * (a) On an Error/CmdError IRQ, pull GetErrors (0x0110) so the ErrorStat bits
 *     (PLL/XOSC/calib/front-end failures, datasheet 6.7.4) are visible instead
 *     of a bare "something errored". OR-accumulated since boot. LR2021_IRQ_ERROR
 *     drives DIO9 (it is masked); CmdError is not masked but still appears in the
 *     GetAndClearIrq word when pending, so we count it opportunistically (it is
 *     also caught per-command by the Stat decode in lr2021_spi_cmd).
 *
 * (b) Once per ~second in RX mode, pull GetFlrcRxStats (0x024A) for the
 *     silicon's own pkt_rx/crc_error/len_error -- ground truth independent of
 *     the host framing path. Throttled so it adds ~1 cmd/s, not 1/IRQ.
 *
 * (c) Read then clear the FIFO flags (RxFifoFlags, TxFifoFlags; datasheet table
 *     6-52) so the next heartbeat counts ONLY overflow/underflow events from the
 *     next interval. Bits per §6.10.1:
 *     0x01 Empty 0x02 Low 0x04 High 0x08 Full 0x10 Overflow 0x20 Underflow. */
static void lr2021_irq_chip_diag_locked(LR2021_Device *dev, u32 irq_status) {
    if (irq_status & LR2021_IRQ_ERROR)
        atomic_inc(&dev->chip_err_count);
    if (irq_status & LR2021_IRQ_CMD_ERROR)
        atomic_inc(&dev->cmd_err_count);
    if (irq_status & (LR2021_IRQ_ERROR | LR2021_IRQ_CMD_ERROR)) {
        u8 err_buf[2] = { 0, 0 };
        if (lr2021_spi_cmd(dev, LR2021_CMD_GET_ERRORS, NULL, 0,
                           err_buf, sizeof(err_buf)) == 0)
            WRITE_ONCE(dev->chip_error_stat,
                       (u16)(READ_ONCE(dev->chip_error_stat) |
                             lr2021_decode_errors(err_buf)));
    }

    if (dev->mode == RADIO_MODE_RX &&
        time_after(jiffies, dev->next_flrc_stats_jiffies)) {
        u8 rxs_buf[6] = { 0 };
        if (lr2021_spi_cmd(dev, LR2021_CMD_GET_FLRC_RX_STATS, NULL, 0,
                           rxs_buf, sizeof(rxs_buf)) == 0) {
            struct lr2021_flrc_rx_stats s;
            lr2021_decode_flrc_rx_stats(rxs_buf, &s);
            WRITE_ONCE(dev->flrc_rxs_pkt, s.pkt_rx);
            WRITE_ONCE(dev->flrc_rxs_crc, s.crc_error);
            WRITE_ONCE(dev->flrc_rxs_len, s.len_error);
        }
        dev->next_flrc_stats_jiffies = jiffies + HZ;
    }

    /* IMP-2c: this Get+Clear pair used to run on EVERY IRQ pass (2 SPI
     * commands, each with a BUSY wait) purely to feed two diagnostic
     * counters. Throttled to ~1 Hz like the FLRC stats poll above. The chip
     * accumulates the flags until cleared, so a 1 Hz sample still catches
     * every tx-underflow event (just up to 1 s late -- fine for an
     * expected-0 alarm). rx-overflow becomes SAMPLED: the accumulated 0x10
     * bit is gated on the sampling pass's RX_FIFO bit, which at 511 B
     * saturation is nearly always set (a drain segment is serviced almost
     * every pass), so the benign drain-pressure trips stay suppressed and a
     * counted overflow still means "overflow with no drain in flight". */
    if (time_after(jiffies, dev->next_fifo_flags_jiffies)) {
        u8 fifo_flags[2] = { 0, 0 };
        u8 clear_args[3] = { 0x03, 0xFF, 0xFF };  /* clear both Rx+Tx, all bits */
        dev->next_fifo_flags_jiffies = jiffies + HZ;
        (void)lr2021_spi_cmd(dev, LR2021_CMD_GET_FIFO_IRQ_FLAGS,
                             NULL, 0, fifo_flags, sizeof(fifo_flags));
        (void)lr2021_spi_cmd(dev, LR2021_CMD_CLEAR_FIFO_IRQ_FLAGS,
                             clear_args, sizeof(clear_args), NULL, 0);
        /* Stash into the device struct so the heartbeat printout (outside the
         * mutex) can see them. Bit 4 (0x10) on Rx side = overflow.
         *
         * Count rx-overflow ONLY on passes that did NOT also service an
         * RxFifo-high drain (LR2021_IRQ_RX_FIFO). A >256 B FLRC packet (prod is
         * 511 B) cannot fit the 256 B HW FIFO, so it is drained in ~128 B
         * segments in-flight (lr2021_drain_rx_during_rx); the host runs ~one
         * segment behind the 2.6 Mbps air fill, so the FIFO repeatedly touches
         * the overflow mark once per segment. MEASURED 2026-06-15: that benign
         * drain-pressure tripped this counter ~4x/packet (== ceil(511/128)) yet
         * lost zero bytes (CRC/seq_drop ~0, payload byte-exact). Gating on the
         * RX_FIFO bit suppresses the expected per-segment trips and leaves the
         * counter meaning what its name claims: an overflow NOT explained by a
         * large-packet drain (e.g. a <=256 B packet overflowing, or the host
         * falling behind outside the drain path) -- a genuine anomaly that the
         * raw ~950/s noise would otherwise mask. Real large-packet data loss
         * still surfaces via crc/sync/len/seq_drop, which are counted elsewhere. */
        if ((fifo_flags[0] & 0x10) && !(irq_status & LR2021_IRQ_RX_FIFO))
            atomic_inc(&dev->rx_fifo_overflow_count);
        if (fifo_flags[1] & 0x20) atomic_inc(&dev->tx_fifo_underflow_count);
    }
}

/* Heartbeat diagnostics: bump the per-error counters and, at a 5 s ratelimit,
 * log the since-boot loss tallies plus chip-side ground truth. Runs OUTSIDE
 * cmd_mutex. The counters are dev-> fields so GET_STATS and debugfs read the
 * same values; logged at heartbeat rate so we can quantify CRC/SYNC/LEN losses
 * without flooding dmesg. dev_dbg is no good here -- DietPi kernels lack
 * CONFIG_DYNAMIC_DEBUG. */
static void lr2021_irq_heartbeat(LR2021_Device *dev, u32 irq_status) {
    static DEFINE_RATELIMIT_STATE(rs, 5 * HZ, 1);

    if (irq_status & LR2021_IRQ_CRC_ERROR) atomic_inc(&dev->crc_count);
    if (irq_status & LR2021_IRQ_SYNC_FAIL) atomic_inc(&dev->sync_count);
    /* LenError = count-only. Probe-confirmed (2026-06-04): the chip
     * auto-discards the over-range packet, leaving the RX FIFO empty and
     * sync self-restoring -- NO ClearRxFifo, no drain. RX configured max
     * pld_len is 511 (B1/B3) so legitimate large packets never trip this;
     * a LenError now means a genuinely corrupted/over-range on-air header.
     * Probe also showed LenError fires only ABOVE the configured max, never
     * below a minimum. See docs/MEASUREMENTS.md "Chip LenError semantics". */
    if (irq_status & LR2021_IRQ_LEN_ERROR) atomic_inc(&dev->len_count);
    if (irq_status & LR2021_IRQ_TIMEOUT)   atomic_inc(&dev->tmout_count);

    if (__ratelimit(&rs)) {
        /* wedge_recoveries packs soft (low 16) + hard (high 16);
         * split here for display. See lr2021_wedge.c. */
        int wr = atomic_read(&dev->wedge_recoveries);
        dev_info(&dev->spi->dev,
                 "rx_err since boot: crc=%d sync=%d len=%d tmout=%d crc_drop=%d ring_full=%llu rx_fifo_ovf=%d tx_fifo_und=%d wedge_soft=%d wedge_hard=%d\n",
                 atomic_read(&dev->crc_count),
                 atomic_read(&dev->sync_count),
                 atomic_read(&dev->len_count),
                 atomic_read(&dev->tmout_count),
                 atomic_read(&dev->crc_drops),
                 (unsigned long long)READ_ONCE(dev->rx_ring_full_drops),
                 atomic_read(&dev->rx_fifo_overflow_count),
                 atomic_read(&dev->tx_fifo_underflow_count),
                 wr & 0xFFFF,
                 (wr >> 16) & 0xFFFF);
        /* Chip-side ground truth + command-status health (second line so
         * the legacy line above stays grep-stable for existing tooling). */
        dev_info(&dev->spi->dev,
                 "chip since boot: cmd_serr=%d chip_err=%d cmd_err=%d err_stat=0x%04x flrc_chip[pkt=%u crc=%u len=%u] mode=%u\n",
                 atomic_read(&dev->cmd_status_err),
                 atomic_read(&dev->chip_err_count),
                 atomic_read(&dev->cmd_err_count),
                 READ_ONCE(dev->chip_error_stat),
                 READ_ONCE(dev->flrc_rxs_pkt),
                 READ_ONCE(dev->flrc_rxs_crc),
                 READ_ONCE(dev->flrc_rxs_len),
                 READ_ONCE(dev->last_chip_mode));
#ifdef COALESCE_PROBE
        dev_info(&dev->spi->dev,
                 "COALPROBE totals: rxdone=%d surplus_evt=%d mislen_evt=%d\n",
                 atomic_read(&coalprobe_rxdone),
                 atomic_read(&coalprobe_surplus),
                 atomic_read(&coalprobe_mislen));
#endif
    }
}

irqreturn_t lr2021_dio9_thread(int irq, void *data) {
    LR2021_Device *dev = data;
    u8  status_buf[4];
    u32 irq_status;
    int ret;

    (void)irq;

    /* Wedge-watchdog liveness stamp. The watchdog timer reads this with
     * READ_ONCE and compares against ktime_get(); a gap larger than
     * LR2021_WEDGE_GRACE_MS while in RX mode triggers recovery. Stamped
     * BEFORE acquiring cmd_mutex so the timestamp advances even on IRQs
     * that race the TX worker and must spin briefly on the mutex. */
    WRITE_ONCE(dev->last_dio9_ktime, ktime_get());

    /* Take cmd_mutex across the whole sequence: GetAndClearIrq plus any
     * follow-up SPI commands (GetRxPktLength + fifo_read for RX_DONE).
     * Locking is caller-side now (see lr2021_spi.h locking contract), so
     * we explicitly serialize against the TX worker here. */
    mutex_lock(&dev->cmd_mutex);

    /* Atomic read-and-clear: one SPI command returns the IRQ status
     * word and clears the pending bits. Avoids the read-and-clear-as-
     * two-commands race (an IRQ that arrives between the read and the
     * clear would be silently dropped). */
    ret = lr2021_spi_cmd(dev, LR2021_CMD_GET_AND_CLEAR_IRQ,
                         NULL, 0, status_buf, sizeof(status_buf));
    if (ret) {
        mutex_unlock(&dev->cmd_mutex);
        dev_warn_ratelimited(&dev->spi->dev,
                             "IRQ status read failed: %d\n", ret);
        lr2021_bump_irq_error(dev);
        return IRQ_HANDLED;
    }

    /* Chip clocks the IRQ register out MSB-first across 4 bytes
     * (datasheet section 6.9.2 / table 6-49 response frame ordering:
     * bits 31:24, 23:16, 15:8, 7:0). Reconstruct the 32-bit value with
     * explicit shifts so we don't depend on host endianness. */
    irq_status = ((u32)status_buf[0] << 24) |
                 ((u32)status_buf[1] << 16) |
                 ((u32)status_buf[2] <<  8) |
                 ((u32)status_buf[3]);

    if (irq_status & LR2021_IRQ_TX_DONE) {
        /* Chain-while-busy: the finished packet's TX_DONE is our cue to submit
         * the next one IN THIS critical section (we already hold cmd_mutex),
         * collapsing the worker-scheduler gap the old blocking worker paid.
         * tx_inflight is already true; submit_locked clears it under tx_lock iff
         * the ring is now empty -- closing the re-prime race against the
         * worker's claim. Legal only because this is a THREADED IRQ in process
         * context. */
#ifdef TXPERF
        WRITE_ONCE(dev->tx_done_ktime, ktime_get());
#endif
        enum lr2021_submit_result r = lr2021_tx_submit_locked(dev);
        (void)r;   /* EMPTY/ERR already cleared tx_inflight under tx_lock;
                    * the worker re-primes on the next TX_ENQUEUE. */
    }

    if (irq_status & LR2021_IRQ_TX_FIFO) {
        /* TX FIFO crossed its low watermark -> top up the in-flight packet.
         * Runs under cmd_mutex (we hold it) so it serialises against
         * submit_locked. No-op once the packet is fully written. */
        lr2021_tx_refill_locked(dev);
    }

    if (irq_status & LR2021_IRQ_RX_FIFO) {
        /* RX FIFO crossed its high watermark -> drain it into the per-packet
         * accumulator so a >256 B payload does not overflow the 256 B FIFO
         * during reception. MUST run before the RX_DONE handler below: if both
         * bits are set in one IRQ (the last drain chunk and packet completion
         * coincide), these bytes have to be appended to the accumulator BEFORE
         * lr2021_drain_rx_packet reads the remainder and finalizes. Runs under
         * cmd_mutex (we hold it); rx_drain_off is single-owner here. No-op for
         * <=256 B packets (this IRQ never fires). */
        lr2021_drain_rx_during_rx(dev);
    }

    if (irq_status & LR2021_IRQ_RX_DONE) {
        bool crc_failed = (irq_status & LR2021_IRQ_CRC_ERROR) != 0;
#ifdef COALESCE_PROBE
        coalprobe_irq_status = irq_status;
#endif
        lr2021_drain_rx_packet(dev, crc_failed);
    } else if (irq_status &
               (LR2021_IRQ_LEN_ERROR | LR2021_IRQ_TIMEOUT | LR2021_IRQ_SYNC_FAIL)) {
        /* Terminal RX abort WITHOUT an RX_DONE: a reception that started and
         * accumulated bytes via RxFifo-high IRQs (rx_drain_off now nonzero) can
         * be killed before completion -- most importantly LEN_ERROR (the chip
         * auto-discards the over-range packet; per the probe note below its FIFO
         * behaviour is datasheet-uncertain), and defensively a TIMEOUT/SYNC_FAIL
         * abort. None of these is followed by an RX_DONE, so the drain-path reset
         * in lr2021_drain_rx_packet never runs and the stale cursor would make
         * the NEXT packet's read start at the wrong offset (misframe/corruption).
         * Reset it here. Guarded by the `else` so we never clobber an in-progress
         * accumulation that WILL still get its RX_DONE; CRC_ERROR is excluded
         * because it arrives together with RX_DONE in this driver and is already
         * handled (and reset) on the drain path above. */
        dev->rx_drain_off = 0;
    }

#ifdef FLRC_PROBE
    if (irq_status & LR2021_IRQ_LEN_ERROR) {
        /* Probe: does a LenError leave the RX FIFO advanced
         * (empty -> chip auto-discarded) or stale (non-empty -> Plan B must
         * ClearRxFifo to keep framing)? Response width of GetRxFifoLevel is
         * datasheet-uncertain, so log the RAW bytes and interpret offline.
         * Two reads bracket any settling. */
        u8 lvl_a[4] = {0};
        u8 lvl_b[4] = {0};
        (void)lr2021_spi_cmd(dev, LR2021_CMD_GET_RX_FIFO_LEVEL, NULL, 0,
                             lvl_a, sizeof(lvl_a));
        (void)lr2021_spi_cmd(dev, LR2021_CMD_GET_RX_FIFO_LEVEL, NULL, 0,
                             lvl_b, sizeof(lvl_b));
        dev_info(&dev->spi->dev,
                 "FLRC_PROBE LenError rxfifo_level raw_a=%4ph raw_b=%4ph\n",
                 lvl_a, lvl_b);
    }
#endif

    lr2021_irq_chip_diag_locked(dev, irq_status);

    mutex_unlock(&dev->cmd_mutex);

    lr2021_irq_heartbeat(dev, irq_status);

    return IRQ_HANDLED;
}
