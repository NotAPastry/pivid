#ifndef TX_TX_SAMPLE_H
#define TX_TX_SAMPLE_H

/* AQM "golden baseline" instrumentation, consolidated into pivid-tx.
 *
 * The driver enforces exclusive open on /dev/radio0 (lr2021_open's
 * atomic_cmpxchg(&is_open, 0, 1)), so a standalone sampler process cannot
 * mmap the ring -- and residency fundamentally needs the in-process enqueue
 * timestamp anyway. So both phases live here, in one thread, gated entirely
 * behind PIVID_TX_SAMPLE: dormant by default (one predictable branch in the
 * push hot path), active only when the env var is set.
 *
 *   - Phase 1 (occupancy): the thread polls (rd, wr) off the SHARED tx_ring
 *     mapping pivid-tx already owns -- rd-then-wr load order so a stale wr
 *     minus a fresh rd can't underflow -- at a 200 Hz absolute-deadline
 *     cadence, logging nominal-vs-actual wake skew as its own jitter signal.
 *   - Phase 2 (residency): tx_push records (wr_at_enqueue, ts, seq) into an
 *     SPSC sidecar under the producer_lock (so the snapshot matches the wr_idx
 *     just published). The same thread drains it: a record's bytes have
 *     drained once rd crosses its end-position (wrap-safe signed-diff). A
 *     stalled consumer is lapped-safe -- it detects the lap, counts the gap,
 *     and skips forward, so a stall shows up as DROP rows, never silent
 *     corruption.
 *
 * Output: one CSV (PIVID_TX_SAMPLE_CSV, default /run/pivid/txsample.csv) with
 * a `kind` column -- OCC / RES / DROP rows share one file and one clock, so
 * there's no cross-process / cross-clock correlation to do.
 */

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

struct TX_Dev;   /* forward decl: tx_sample_start wires dev->sample + tx_ring */

#define TXSAMPLE_CAP 512u   /* SPSC sidecar depth (~2 s at ~250 rec/s) */

typedef struct {
    uint32_t wr_at_enqueue;  /* free-running wr_idx AFTER this record committed */
    uint64_t ts_ns;          /* CLOCK_MONOTONIC ns at enqueue                  */
    uint64_t seq;            /* monotonic record number (== producer index)    */
} TxSampleRec;

typedef struct TxSample {
    int        active;       /* 0 => fully dormant; all hooks no-op (atomic)   */
    void      *tx_ring;      /* SHARED mmap (not owned); NULL when dormant      */
    FILE      *csv;
    pthread_t  thread;
    volatile sig_atomic_t stop;

    /* SPSC sidecar. Single serialized producer: tx_push, always under
     * producer_lock (the encoder and MSP poller both push, but never
     * concurrently). Single consumer: the instrumenter thread. */
    TxSampleRec ring[TXSAMPLE_CAP];
    uint64_t    prod_idx;    /* producer-owned; published RELEASE              */
    uint64_t    cons_idx;    /* consumer-owned                                 */
    uint64_t    dropped;     /* residency entries lost to producer lapping     */
} TxSample;

/* Reads PIVID_TX_SAMPLE. If unset/empty/"0": zero-init `s`, leave it dormant,
 * set dev->sample = s (hooks no-op), return 0. If set: open the CSV, wire
 * dev->sample + tx_ring, and spawn the instrumenter thread. Returns 0 on
 * success (dormant or active), -1 only if an explicitly-requested sampler
 * failed to start (CSV open / pthread_create). */
int  tx_sample_start(TxSample *s, struct TX_Dev *dev);

/* Stop + join the thread, flush + close the CSV, clear dev->sample. Idempotent;
 * a no-op if never started. Call from main() shutdown BEFORE tx_close. */
void tx_sample_stop(TxSample *s, struct TX_Dev *dev);

/* Producer hook -- called from tx_push under producer_lock, right after
 * ring_user_push() succeeds, with the just-published wr_idx. Inline so the
 * dormant path is a single branch with no call overhead. `s` is NULL until
 * main wires it, and `active` is an acquire load so a producer that observes
 * active==1 also observes the zero-initialised sidecar. */
static inline void tx_sample_record(TxSample *s, uint32_t wr_after) {
    if (!s || !__atomic_load_n(&s->active, __ATOMIC_ACQUIRE)) return;
    uint64_t p = s->prod_idx;                       /* producer-owned */
    TxSampleRec *r = &s->ring[p % TXSAMPLE_CAP];
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    r->wr_at_enqueue = wr_after;
    r->ts_ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    r->seq   = p;
    /* Publish: slot contents must be visible before the index advances. */
    __atomic_store_n(&s->prod_idx, p + 1, __ATOMIC_RELEASE);
}

#endif /* TX_TX_SAMPLE_H */
