/* AQM baseline instrumenter -- see tx_sample.h for the architecture rationale. */

#define _POSIX_C_SOURCE 200809L

#include "tx_sample.h"
#include "tx.h"

#include "ring_user.h"      /* ring_wr_ptr / ring_rd_ptr accessors        */
#include "radio_ioctl.h"    /* RADIO_RING_DATA_BYTES                       */
#include "tx_csv.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PERIOD_NS      5000000L   /* 5 ms -> 200 Hz                        */
#define THERMAL_PATH   "/sys/class/thermal/thermal_zone0/temp"
#define THERMAL_EVERY  200        /* 200 ticks * 5 ms = 1 Hz               */
#define CSV_DEFAULT    "/run/pivid/txsample.csv"

static inline uint64_t ts_ns(struct timespec t) {
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Die temp in milli-degrees C, or -1 if the node can't be read. Polled at
 * 1 Hz only -- a sysfs open/read at 200 Hz would inject the very jitter the
 * wake_skew column exists to measure. */
static int read_thermal_mC(void) {
    int fd = open(THERMAL_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (int)strtol(buf, NULL, 10);
}

static void *tx_sample_thread(void *arg) {
    TxSample *s = arg;
    struct timespec next, now;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int thermal_tick = THERMAL_EVERY;   /* read on the first tick */
    int temp = -1;

    while (!__atomic_load_n(&s->stop, __ATOMIC_RELAXED)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ns = ts_ns(now);

        /* --- occupancy: rd BEFORE wr so used = wr - rd can't underflow --- */
        uint32_t rd  = __atomic_load_n(ring_rd_ptr(s->tx_ring), __ATOMIC_ACQUIRE);
        uint32_t wr  = __atomic_load_n(ring_wr_ptr(s->tx_ring), __ATOMIC_RELAXED);
        uint32_t used = wr - rd;
        unsigned occ_pct = (unsigned)((uint64_t)used * 100ull / RADIO_RING_DATA_BYTES);

        if (++thermal_tick >= THERMAL_EVERY) { temp = read_thermal_mC(); thermal_tick = 0; }

        long skew = (long)((int64_t)now_ns - (int64_t)ts_ns(next));  /* wake jitter */
        fprintf(s->csv, "OCC,%llu,%u,%u,%d,%ld,0,0\n",
                (unsigned long long)now_ns, used, occ_pct, temp, skew);

        /* --- residency drain (in-order; FIFO since wr_at_enqueue is monotonic
         *     under the producer_lock) --- */
        uint64_t head = __atomic_load_n(&s->prod_idx, __ATOMIC_ACQUIRE);
        uint64_t lag  = head - s->cons_idx;            /* uint64: no wrap in a run */
        if (lag > TXSAMPLE_CAP) {                      /* producer lapped us */
            uint64_t keep = TXSAMPLE_CAP - 1;
            s->dropped  += lag - keep;
            s->cons_idx  = head - keep;                /* skip to freshest safe slot */
            fprintf(s->csv, "DROP,%llu,0,0,0,0,%llu,0\n",
                    (unsigned long long)now_ns, (unsigned long long)s->dropped);
        }
        while (s->cons_idx < head) {
            TxSampleRec *r = &s->ring[s->cons_idx % TXSAMPLE_CAP];
            /* Drained once rd has reached/passed this record's end position.
             * Signed-diff is wrap-safe; |rd - wr_at_enqueue| <= ring size (32 KB)
             * is far under INT32_MAX so the cast is unambiguous. */
            if ((int32_t)(rd - r->wr_at_enqueue) >= 0) {
                long res_ns = (long)((int64_t)now_ns - (int64_t)r->ts_ns);
                fprintf(s->csv, "RES,%llu,0,0,0,0,%llu,%ld\n",
                        (unsigned long long)now_ns,
                        (unsigned long long)r->seq, res_ns);
                s->cons_idx++;
            } else {
                break;   /* this one hasn't drained -> none after it have either */
            }
        }

        fflush(s->csv);   /* tmpfs target -- cheap; keeps the capture readable live */

        /* Advance the absolute deadline; PERIOD_NS < 1 s so one carry suffices. */
        next.tv_nsec += PERIOD_NS;
        if (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

int tx_sample_start(TxSample *s, struct TX_Dev *dev) {
    memset(s, 0, sizeof(*s));
    dev->sample = s;   /* wire the hook target even when dormant (it no-ops) */

    const char *en = getenv("PIVID_TX_SAMPLE");
    if (!en || !*en || (en[0] == '0' && en[1] == '\0')) {
        return 0;   /* dormant: hooks see active==0 and return immediately */
    }

    const char *path = getenv("PIVID_TX_SAMPLE_CSV");
    if (!path || !*path) path = CSV_DEFAULT;
    s->csv = txcsv_acquire(path);   /* shared with aqm; header written once */
    if (!s->csv) { perror("tx_sample: txcsv_acquire"); return -1; }

    s->tx_ring = dev->tx_ring;
    /* RELEASE so a producer that later observes active==1 also observes the
     * zero-initialised sidecar + tx_ring written above. */
    __atomic_store_n(&s->active, 1, __ATOMIC_RELEASE);

    if (pthread_create(&s->thread, NULL, tx_sample_thread, s) != 0) {
        perror("tx_sample: pthread_create");
        __atomic_store_n(&s->active, 0, __ATOMIC_RELEASE);
        txcsv_release();
        s->csv = NULL;
        return -1;
    }
    fprintf(stderr, "pivid-tx: TX sampler ACTIVE -> %s\n", path);
    return 0;
}

void tx_sample_stop(TxSample *s, struct TX_Dev *dev) {
    if (__atomic_load_n(&s->active, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s->stop, 1, __ATOMIC_RELAXED);
        pthread_join(s->thread, NULL);
        __atomic_store_n(&s->active, 0, __ATOMIC_RELEASE);   /* hooks no-op now */
        if (s->csv) { fflush(s->csv); txcsv_release(); s->csv = NULL; }
        if (s->dropped) {
            fprintf(stderr, "pivid-tx: TX sampler dropped %llu residency samples "
                            "(consumer lapped)\n", (unsigned long long)s->dropped);
        }
    }
    if (dev) dev->sample = NULL;
}
