/* Steady-state wedge watchdog for the LR2021.
 *
 * Symptom (observed 2026-05-30): after 2-7 minutes of continuous RX
 * operation, the chip stops asserting DIO9 even though SPI remains
 * responsive (GET_RSSI ioctl still works). RX is dead until
 * rmmod + modprobe (which pulses NRESET via hw_reset).
 *
 * Recovery ladder, cheapest first:
 *   1. Soft re-arm: SetStandby(RC) + SetRx(continuous). Mirrors the
 *      same sequence used by RADIO_IOCTL_SET_MODE(RX) and already
 *      known to bring the chip back from at least one stuck state
 *      (the userspace-restart case fixed in 306e836).
 *   2. Hard reset: NRESET pulse + full hw_init + SetRx. The heavy
 *      hammer that always works.
 *
 * RX chip-liveness gate (2026-07-06): DIO9 silence alone cannot
 * distinguish "chip wedged" from "RF just quiet", and the un-gated recovery
 * blanked a live receiver on every >GRACE_MS lull (repeating every ~GRACE ms
 * for the whole lull -- 18 spurious soft recovers in one bench arm). Now, once
 * silence passes GRACE/2, each poll tick reads GetFlrcRxStats (the silicon's
 * own pkt/crc/len counters -- ground truth independent of the DIO9 path); the
 * first read baselines the episode. At GRACE the verdict:
 *   - counters frozen + chip in Rx + RX FIFO empty  => RF quiet, chip provably
 *     fine: SKIP recovery (count in wedge_quiet_skips, once per episode).
 *   - anything else (counters advanced = chip demodulating while DIO9 silent,
 *     i.e. an IRQ-delivery stall; mode fell out of Rx; FIFO residue with no
 *     RX_DONE coming; stats unreadable) => recover, logging the full diagnosis
 *     (counter deltas, chip mode, FIFO level, DIO9 GPIO level) so recurring
 *     stalls are classifiable from dmesg.
 * wedge_chip_gate=0 restores the legacy always-recover behavior at runtime
 * (the diagnosis logging stays).
 *
 * Runs in a delayed_work (process context), not a kernel timer
 * (softirq). Required: try_soft_recover takes cmd_mutex and
 * lr2021_spi_cmd calls spi_sync() + usleep_range(), all of which
 * sleep -- illegal in softirq, observed as "BUG: scheduling while
 * atomic" when the v1 timer-based implementation was deployed.
 *
 * The work re-arms itself at 1-second cadence so the wedge is caught
 * within ~1 s of LR2021_WEDGE_GRACE_MS. Soft path counted separately
 * from hard path via dev->wedge_recoveries (low 16 bits soft, high 16
 * bits hard) so the heartbeat can report both without an extra atomic. */
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/ratelimit.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include "lr2021.h"
#include "lr2021_commands.h"
#include "lr2021_spi.h"

/* Poll cadence. RX wedges are caught within ~1s of LR2021_WEDGE_GRACE_MS; TX
 * wedges need a tighter poll to honour LR2021_TX_WEDGE_GRACE_MS (50 ms). 25 ms
 * catches a TX wedge within ~75 ms while staying cheap (one SPI-free ktime
 * compare per tick; recovery SPI only fires on an actual wedge). */
static unsigned int wedge_poll_ms = 25;
module_param(wedge_poll_ms, uint, 0644);
MODULE_PARM_DESC(wedge_poll_ms, "wedge watchdog poll period (ms)");

/* RX chip-liveness gate (see header comment). Runtime-writable so a bench
 * A/B can flip legacy/gated behavior without a module reload. */
static bool wedge_chip_gate = true;
module_param(wedge_chip_gate, bool, 0644);
MODULE_PARM_DESC(wedge_chip_gate,
                 "skip RX soft-recover when chip counters prove RF-quiet (0=legacy always-recover)");

/* SetRx continuous: rx_timeout = 0xFFFFFF (datasheet 6.3.5). Identical
 * to the constant in lr2021_apply_mode -- keeping a local copy avoids
 * exporting it just for the watchdog. */
static const u8 SET_RX_CONTINUOUS[3] = { 0xFF, 0xFF, 0xFF };

/* try_soft_recover() return sentinel (positive, so it can't collide with the
 * 0/-errno that lr2021_spi_cmd returns): the TX wedge decision raced a fresh
 * submit -- last_submit_ktime advanced under cmd_mutex between the snapshot and
 * the recovery -- so recovery was skipped. The caller leaves tx state intact. */
#define LR2021_WEDGE_SOFT_RACED 1

static inline void bump_soft(LR2021_Device *dev) {
    atomic_add(1, &dev->wedge_recoveries);
}
static inline void bump_hard(LR2021_Device *dev) {
    atomic_add(1 << 16, &dev->wedge_recoveries);
}

/* A recovery (SetStandby, or NRESET pulse + hw_init) flushes the chip RX FIFO.
 * Clear the host's mid-RX drain cursor to match, so an in-progress >256 B
 * reception torn down by the recovery can't leave a stale offset that misframes
 * the first post-recovery packet. Caller must hold cmd_mutex (the DIO9 thread
 * is the only other writer of rx_drain_off). */
static inline void lr2021_clear_rx_drain_cursor(LR2021_Device *dev) {
    dev->rx_drain_off = 0;
}

/* Drive SetStandby(RC) followed by SetRx(continuous). Mirrors the
 * apply_mode(RX) path; safe to call while another caller has already
 * placed the chip in RX (the chip accepts SetStandby from any state).
 *
 * guard_submit closes the TX wedge-vs-IRQ recovery race. The TX caller took
 * its wedge decision on a last_submit_ktime snapshot read under tx_lock, then
 * dropped tx_lock to get here. In that window the DIO9 threaded IRQ can win
 * cmd_mutex first and submit a fresh packet (new SetTx, new last_submit_ktime)
 * or drain the ring to idle (last_submit_ktime -> 0). The IRQ's submit_locked
 * stamps last_submit_ktime under cmd_mutex, and we re-read it under the SAME
 * cmd_mutex here, so a value different from the snapshot PROVES the chip made
 * forward progress and is NOT wedged -- aborting recovery prevents us from
 * SetStandby-ing a live packet out from under it and force-clearing the
 * just-popped record (a silent TX drop). Pass guard_submit == 0 to disable the
 * guard (the RX liveness caller, which has no TX submit concept).
 *
 * guard_dio9 is the same idea for the RX caller: its wedge decision was taken
 * on a last_dio9_ktime snapshot. The DIO9 thread stamps last_dio9_ktime BEFORE
 * taking cmd_mutex, so re-reading it under the SAME cmd_mutex here proves
 * whether an IRQ landed in the decision->recover window -- if it did, the chip
 * is delivering again and a SetStandby would chop the packet being serviced.
 * Pass 0 to disable (the TX caller). */
static int try_soft_recover(LR2021_Device *dev, ktime_t guard_submit,
                            ktime_t guard_dio9) {
    const u8 standby_rc = LR2021_STANDBY_MODE_RC;
    int ret;

    mutex_lock(&dev->cmd_mutex);
    if (ktime_to_ns(guard_submit) != 0 &&
        READ_ONCE(dev->last_submit_ktime) != guard_submit) {
        mutex_unlock(&dev->cmd_mutex);
        return LR2021_WEDGE_SOFT_RACED;
    }
    if (ktime_to_ns(guard_dio9) != 0 &&
        READ_ONCE(dev->last_dio9_ktime) != guard_dio9) {
        mutex_unlock(&dev->cmd_mutex);
        return LR2021_WEDGE_SOFT_RACED;
    }
    ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_STANDBY,
                         &standby_rc, sizeof(standby_rc), NULL, 0);
    if (ret == 0)
        ret = lr2021_spi_cmd(dev, LR2021_CMD_SET_RX,
                             SET_RX_CONTINUOUS, sizeof(SET_RX_CONTINUOUS),
                             NULL, 0);
    lr2021_clear_rx_drain_cursor(dev);
    mutex_unlock(&dev->cmd_mutex);
    return ret;
}

/* NRESET pulse + hw_init + SetRx. Locking mirrors RADIO_IOCTL_RESET:
 * cmd_mutex is held across the NRESET pulse (so the IRQ thread can't
 * observe the chip mid-reset) and released before hw_init, which takes
 * the mutex itself. */
static void try_hard_recover(LR2021_Device *dev) {
    mutex_lock(&dev->cmd_mutex);
    lr2021_hw_reset(dev);
    mutex_unlock(&dev->cmd_mutex);

    (void)lr2021_hw_init(dev);

    /* hw_init leaves the chip in Standby RC. dev->mode is still
     * RADIO_MODE_RX from userspace's perspective (we never touched it),
     * so re-issue SetRx here rather than bouncing through the ioctl. */
    mutex_lock(&dev->cmd_mutex);
    (void)lr2021_spi_cmd(dev, LR2021_CMD_SET_RX,
                         SET_RX_CONTINUOUS, sizeof(SET_RX_CONTINUOUS),
                         NULL, 0);
    lr2021_clear_rx_drain_cursor(dev);
    mutex_unlock(&dev->cmd_mutex);
}

static void wedge_work_fn(struct work_struct *work) {
    LR2021_Device *dev = container_of(to_delayed_work(work),
                                      LR2021_Device, wedge_dwork);
    ktime_t now, last;
    s64 gap_ms;

    /* The RX chip-liveness probe below is only meaningful while we stay in
     * RX mode; a mode excursion (TX/bind/standby) leaves a stale baseline
     * that could mis-compare against counters from a different RX session.
     * All probe fields are single-writer (this work), so plain stores. */
    if (dev->mode != RADIO_MODE_RX) {
        dev->wedge_probe_valid = false;
        dev->wedge_quiet_noted = false;
    }

    /* Watch RX (no DIO9 in over GRACE = chip stopped delivering) and TX (a
     * packet in flight longer than TX grace = SetTx/TX_DONE wedged). Bind modes
     * are short-lived; skip. dev->mode is updated under cmd_mutex; a racy read
     * here is fine (worst case one skipped/extra tick). */
    if (dev->mode == RADIO_MODE_TX || dev->mode == RADIO_MODE_BIND_TRANSMIT) {
        unsigned long flags;
        bool    inflight;
        ktime_t last_submit;
        s64     tx_gap_ms;
        int     sr;

        /* Sample (inflight, last_submit) under the SAME tx_lock so the pair is
         * coherent: the clear sites zero last_submit together with
         * tx_inflight=false under this lock, so we can never observe a stale
         * prior-burst timestamp paired with a fresh claim. (The success stamp
         * at tx.c writes last_submit under cmd_mutex, not tx_lock, so keep
         * READ_ONCE to prevent tearing of that lockless write.) */
        spin_lock_irqsave(&dev->tx_lock, flags);
        inflight    = dev->tx_inflight;
        last_submit = READ_ONCE(dev->last_submit_ktime);
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        if (!inflight)
            goto rearm;               /* idle TX: nothing to wedge on. */

        if (ktime_to_ns(last_submit) == 0)
            goto rearm;               /* claimed but not yet submitted. */

        tx_gap_ms = ktime_ms_delta(ktime_get(), last_submit);
        if (tx_gap_ms < LR2021_TX_WEDGE_GRACE_MS)
            goto rearm;

        dev_warn_ratelimited(&dev->spi->dev,
            "TX wedge: in-flight %lld ms with no TX_DONE; recovering\n",
            tx_gap_ms);

        /* Same recovery ladder as RX, but guarded against the IRQ recovery
         * race: pass the last_submit snapshot so try_soft_recover bails (under
         * cmd_mutex) if a fresh packet was submitted in the snapshot->recover
         * window. RACED => the chip is live, not wedged; leave tx_inflight and
         * last_submit untouched and re-arm (the next tick re-snapshots a fresh,
         * sub-grace timestamp, so this does not re-fire). */
        sr = try_soft_recover(dev, last_submit, 0);
        if (sr == LR2021_WEDGE_SOFT_RACED)
            goto rearm;

        /* Genuine wedge. After recovery the chip is in Standby RC; clear
         * tx_inflight so the next TX_ENQUEUE re-primes cleanly, and clear
         * last_submit so the re-primed chain isn't flagged on a stale timestamp
         * (consistent: inflight=false => last_submit=0; the next TX_ENQUEUE
         * re-primes and stamps a fresh one on its first SetTx). */
        if (sr == 0) {
            bump_soft(dev);
        } else {
            dev_warn_ratelimited(&dev->spi->dev,
                "TX wedge soft-recover failed; escalating to NRESET\n");
            try_hard_recover(dev);
            bump_hard(dev);
        }
        {
            unsigned long f;
            spin_lock_irqsave(&dev->tx_lock, f);
            dev->tx_inflight = false;
            WRITE_ONCE(dev->last_submit_ktime, 0);
            spin_unlock_irqrestore(&dev->tx_lock, f);
        }
        goto rearm;
    }

    /* Only watch RX liveness below. TX-only/bind handled above; other modes
     * don't fire DIO9 except on TxDone (worker-driven, bounded). dev->mode is
     * updated under cmd_mutex; a racy read here is fine -- worst case we skip
     * one tick or include one extra. */
    if (dev->mode != RADIO_MODE_RX)
        goto rearm;

    now = ktime_get();
    last = READ_ONCE(dev->last_dio9_ktime);
    if (ktime_to_ns(last) == 0) {
        /* Never received an IRQ yet -- userspace just entered RX, give
         * the link a chance to come up. */
        goto rearm;
    }

    gap_ms = ktime_ms_delta(now, last);
    if (gap_ms < LR2021_WEDGE_GRACE_MS / 2) {
        /* DIO9 alive (or the lull is too short to matter): close any probe
         * episode so the next one baselines afresh. */
        dev->wedge_probe_valid = false;
        dev->wedge_quiet_noted = false;
        goto rearm;
    }

    /* Probe zone (DIO9 silent >= GRACE/2): read the chip's own FLRC RX
     * counters. The first read of an episode is the baseline; arming it at
     * GRACE/2 means the recovery decision at GRACE already has a comparison
     * window, so a genuine stall is recovered at the same ~GRACE+poll latency
     * as the un-gated code. Cost: one SPI command per 25 ms tick, paid only
     * while the link is silent (any reception resets the gap). 16-bit chip
     * counters can't alias across an episode: a wrap needs 65536 receptions,
     * each of which would have fired DIO9 and closed the episode. */
    {
        struct lr2021_flrc_rx_stats s;
        u8   rxs[6];
        u8   lvl[2];
        int  sret, lret = -1;
        u16  level = 0;
        u8   mode = 0;
        int  sr;

        mutex_lock(&dev->cmd_mutex);
        sret = lr2021_spi_cmd(dev, LR2021_CMD_GET_FLRC_RX_STATS,
                              NULL, 0, rxs, sizeof(rxs));
        if (sret == 0 && gap_ms >= LR2021_WEDGE_GRACE_MS) {
            /* Same 2-byte MSB-first response as lr2021_drain_rx_during_rx. */
            lret = lr2021_spi_cmd(dev, LR2021_CMD_GET_RX_FIFO_LEVEL,
                                  NULL, 0, lvl, sizeof(lvl));
            if (lret == 0)
                level = ((u16)lvl[0] << 8) | (u16)lvl[1];
        }
        mutex_unlock(&dev->cmd_mutex);

        if (sret == 0) {
            bool advanced;

            lr2021_decode_flrc_rx_stats(rxs, &s);
            /* Stamped by lr2021_spi_cmd from the response Stat bytes of the
             * commands just issued -- the chip's CURRENT self-reported mode. */
            mode = READ_ONCE(dev->last_chip_mode);

            if (!dev->wedge_probe_valid) {
                dev->wedge_probe       = s;
                dev->wedge_probe_valid = true;
                goto rearm;   /* baseline armed; verdicts from the next tick */
            }
            advanced = s.pkt_rx    != dev->wedge_probe.pkt_rx ||
                       s.crc_error != dev->wedge_probe.crc_error ||
                       s.len_error != dev->wedge_probe.len_error;

            if (gap_ms < LR2021_WEDGE_GRACE_MS)
                goto rearm;   /* armed; grace not yet expired */

            if (wedge_chip_gate && !advanced &&
                mode == LR2021_CHIPMODE_RX && lret == 0 && level == 0) {
                /* RF quiet, chip provably fine: in Rx, FIFO empty, its own
                 * counters frozen across the episode. Recovering here is the
                 * original defect (blanks a live receiver and can chop the
                 * first packet after the lull). Skip, once-per-episode. */
                if (!dev->wedge_quiet_noted) {
                    dev->wedge_quiet_noted = true;
                    atomic_inc(&dev->wedge_quiet_skips);
                    dev_info_ratelimited(&dev->spi->dev,
                        "RX quiet %lld ms (chip pkt=%u frozen, mode=%u, fifo empty); recovery skipped\n",
                        gap_ms, s.pkt_rx, mode);
                }
                goto rearm;
            }

            /* Stall diagnosis breadcrumb (stall RCA): deltas are vs the
             * episode baseline taken at ~GRACE/2 into the silence. dpkt>0
             * with DIO9 silent = the chip demodulated packets the host never
             * saw an IRQ for; dio9 is the raw GPIO level at decision time. */
            dev_warn_ratelimited(&dev->spi->dev,
                "RX wedge: no DIO9 %lld ms; chip dpkt=%u dcrc=%u dlen=%u mode=%u fifo=%u(ret %d) dio9=%d gate=%d; soft recovering\n",
                gap_ms,
                (unsigned)(u16)(s.pkt_rx    - dev->wedge_probe.pkt_rx),
                (unsigned)(u16)(s.crc_error - dev->wedge_probe.crc_error),
                (unsigned)(u16)(s.len_error - dev->wedge_probe.len_error),
                mode, level, lret,
                gpiod_get_value_cansleep(dev->dio9),
                (int)wedge_chip_gate);
        } else {
            if (gap_ms < LR2021_WEDGE_GRACE_MS)
                goto rearm;   /* SPI hiccup before grace: retry next tick */
            /* Stats unreadable at grace: the SPI/chip side is sick; take the
             * legacy ladder (the soft attempt will fail too -> NRESET). */
            dev_warn_ratelimited(&dev->spi->dev,
                "RX wedge: no DIO9 %lld ms and GetFlrcRxStats failed (%d); soft recovering\n",
                gap_ms, sret);
        }

        dev->wedge_probe_valid = false;
        dev->wedge_quiet_noted = false;

        /* guard_dio9=last: abort (RACED) if an IRQ stamped a fresh timestamp
         * between the verdict above and cmd_mutex acquisition -- the chip is
         * delivering again and recovery would chop the packet being serviced. */
        sr = try_soft_recover(dev, 0, last);
        if (sr == LR2021_WEDGE_SOFT_RACED)
            goto rearm;
        if (sr == 0) {
            bump_soft(dev);
            /* Reset last_dio9_ktime to a FRESH timestamp (not `now`, which
             * predates the recover). An IRQ that fired during the recover
             * already wrote a newer ktime_get() into last_dio9_ktime; using
             * `now` here would clobber it with an older value and could
             * cause a spurious wedge re-trigger on the next tick. */
            WRITE_ONCE(dev->last_dio9_ktime, ktime_get());
            goto rearm;
        }

        /* Soft SPI itself returned error -- chip is fully wedged. */
        dev_warn_ratelimited(&dev->spi->dev,
            "wedge soft-recover SPI failed; escalating to NRESET\n");
        try_hard_recover(dev);
        bump_hard(dev);
        WRITE_ONCE(dev->last_dio9_ktime, ktime_get());
    }

rearm:
    schedule_delayed_work(&dev->wedge_dwork,
                          msecs_to_jiffies(wedge_poll_ms));
}

void lr2021_wedge_init(LR2021_Device *dev) {
    INIT_DELAYED_WORK(&dev->wedge_dwork, wedge_work_fn);
    schedule_delayed_work(&dev->wedge_dwork,
                          msecs_to_jiffies(wedge_poll_ms));
}

void lr2021_wedge_destroy(LR2021_Device *dev) {
    /* cancel_delayed_work_sync handles the self-rearming case: if the
     * callback is already running and re-arms via schedule_delayed_work,
     * this routine loops until the work is gone for good. */
    cancel_delayed_work_sync(&dev->wedge_dwork);
}
