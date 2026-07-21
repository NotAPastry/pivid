#define _POSIX_C_SOURCE 200809L
#include "frame_gate.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static int g_failures = 0;
#define EXPECT(c,m) do{ if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,m);g_failures++;} }while(0)

/* Wall-elapsed in ms, measured with CLOCK_MONOTONIC regardless of the gate's
 * internal clock (we measure real elapsed time, not the gate deadline). */
static int64_t mono_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
/* The producer sequence frame_slot_publish runs inline: bump + signal under lock. */
static void gate_bump(FrameGate *g){
    pthread_mutex_lock(&g->lock);
    g->generation++;
    pthread_cond_signal(&g->cond);
    pthread_mutex_unlock(&g->lock);
}

/* (1) Returns immediately when the generation has already advanced past seen. */
static void test_immediate_when_advanced(void){
    FrameGate g; frame_gate_init(&g);
    gate_bump(&g);                                  /* generation: 0 -> 1 */
    int64_t t0 = mono_ms();
    uint64_t cur = frame_gate_wait(&g, 0, 5000000000LL);   /* seen=0, 5s timeout */
    int64_t dt = mono_ms() - t0;
    EXPECT(cur == 1, "returns the advanced generation");
    EXPECT(dt < 500, "did not block (returned well under the 5s timeout)");
    /* timeout_ns <= 0 also returns immediately, generation == seen. */
    uint64_t cur0 = frame_gate_wait(&g, 1, 0);
    EXPECT(cur0 == 1, "timeout<=0 polls: returns current generation without blocking");
    frame_gate_destroy(&g);
}

/* (2) Times out at ~timeout_ns when no publish arrives. */
static void test_timeout(void){
    FrameGate g; frame_gate_init(&g);
    int64_t t0 = mono_ms();
    uint64_t cur = frame_gate_wait(&g, 0, 80000000LL);     /* seen=0, 80ms */
    int64_t dt = mono_ms() - t0;
    EXPECT(cur == 0, "generation unchanged on timeout");
    EXPECT(dt >= 60 && dt <= 400, "returned at ~the 80ms timeout (band 60-400ms)");
    frame_gate_destroy(&g);
}

/* (3) A producer thread's bump wakes the waiter well before the timeout. */
static void *bump_after_30ms(void *arg){
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 30000000L };
    nanosleep(&ts, NULL);
    gate_bump((FrameGate*)arg);
    return NULL;
}
static void test_wake_on_bump(void){
    FrameGate g; frame_gate_init(&g);
    pthread_t th; pthread_create(&th, NULL, bump_after_30ms, &g);
    int64_t t0 = mono_ms();
    uint64_t cur = frame_gate_wait(&g, 0, 5000000000LL);   /* seen=0, 5s timeout */
    int64_t dt = mono_ms() - t0;
    pthread_join(th, NULL);
    EXPECT(cur == 1, "woke with the new generation");
    EXPECT(dt < 1000, "woke on the bump (~30ms), well under the 5s timeout");
    frame_gate_destroy(&g);
}

int main(void){
    test_immediate_when_advanced();
    test_timeout();
    test_wake_on_bump();
    if(g_failures){ fprintf(stderr,"test_frame_gate: %d failure(s)\n", g_failures); return 1; }
    printf("test_frame_gate: OK\n"); return 0;
}
