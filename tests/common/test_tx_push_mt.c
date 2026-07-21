/* Multi-threaded correctness test for the tx_push mutex+ring_user_push pattern.
 *
 * Two producer threads each push PUSHES_PER_THREAD 8-byte records into a single
 * heap-allocated ring, serialized by a producer mutex (mirroring what tx_push
 * does so the encoder and the MSP poller can push concurrently). One consumer
 * drains the ring. Verifies every record arrives exactly once with no tearing,
 * and that each thread's records stay in push order.
 *
 * The ring itself is SPSC: the mutex collapses the two producers into one
 * logical producer, so the lock-free consumer is correct. Uses
 * aligned_alloc(4096, RADIO_RING_BYTES) like test_ring_user.c, because the
 * ring layout (header page + data area) requires page alignment. */

#define _POSIX_C_SOURCE 200809L
#include "ring_user.h"
#include "radio_ioctl.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } \
} while (0)

#define PUSHES_PER_THREAD 10000
#define RECORD_LEN 8

static pthread_mutex_t prod_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_ring;

static void *producer(void *arg) {
    uint32_t tid = (uint32_t)(uintptr_t)arg;
    for (int i = 0; i < PUSHES_PER_THREAD; i++) {
        uint8_t rec[RECORD_LEN];
        uint32_t seq = (uint32_t)i;
        memcpy(&rec[0], &tid, 4);
        memcpy(&rec[4], &seq, 4);
        for (;;) {
            pthread_mutex_lock(&prod_lock);
            int rc = ring_user_push(g_ring, rec, sizeof rec);
            pthread_mutex_unlock(&prod_lock);
            if (rc == 0) break;
            sched_yield();   /* ring full -- consumer is draining */
        }
    }
    return NULL;
}

int main(void) {
    g_ring = aligned_alloc(4096, RADIO_RING_BYTES);
    if (!g_ring) { fprintf(stderr, "aligned_alloc failed\n"); return 1; }
    memset(g_ring, 0, RADIO_RING_BYTES);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, producer, (void *)(uintptr_t)1);
    pthread_create(&t2, NULL, producer, (void *)(uintptr_t)2);

    int seen[3] = {0,0,0};
    int last[3] = {-1,-1,-1};
    uint8_t out[RECORD_LEN];
    int total = 0;
    while (total < 2 * PUSHES_PER_THREAD) {
        int n = ring_user_pop(g_ring, out, sizeof out);
        if (n == 0) { sched_yield(); continue; }
        EXPECT(n == RECORD_LEN, "record length unchanged");
        if (n != RECORD_LEN) break;   /* corrupt/torn record (-1 or wrong len): stop */
        uint32_t tid, seq;
        memcpy(&tid, &out[0], 4);
        memcpy(&seq, &out[4], 4);
        EXPECT(tid == 1 || tid == 2, "thread tag in {1,2}");
        if (tid != 1 && tid != 2) break;   /* corrupt tag -- avoid OOB index */
        EXPECT((int)seq > last[tid], "per-thread seq monotonic (no reorder within a thread)");
        last[tid] = (int)seq;
        seen[tid]++;
        total++;
    }
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    EXPECT(seen[1] == PUSHES_PER_THREAD, "all of thread 1's records arrived once");
    EXPECT(seen[2] == PUSHES_PER_THREAD, "all of thread 2's records arrived once");

    free(g_ring);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("OK (20000 records, no tears, ordering preserved)\n");
    return 0;
}
