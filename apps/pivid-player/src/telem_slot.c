/* apps/pivid-player/src/telem_slot.c -- see telem_slot.h. Seqlock identical in
 * shape to apps/pivid-rx/src/msp_telemetry.c's writer/reader. */
#include "telem_slot.h"
#include <string.h>

void telem_slot_init(TelemSlot *t) {
    atomic_store(&t->seq, 0);
    osd_grid_clear(&t->grid);
    atomic_store(&t->last_recv_ns, 0);
}

void telem_slot_publish(TelemSlot *t, const osd_grid_t *grid, int64_t recv_ns) {
    unsigned seq = atomic_load_explicit(&t->seq, memory_order_relaxed);
    atomic_store_explicit(&t->seq, seq + 1, memory_order_relaxed);  /* odd: writing */
    atomic_thread_fence(memory_order_release);
    t->grid = *grid;
    atomic_store_explicit(&t->last_recv_ns, recv_ns, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&t->seq, seq + 2, memory_order_relaxed);  /* even: stable */
}

void telem_slot_read(const TelemSlot *t, osd_grid_t *out,
                     int64_t *out_recv_ns) {
    unsigned s0, s1;
    do {
        s0 = atomic_load_explicit(&t->seq, memory_order_acquire);
        *out = t->grid;
        *out_recv_ns = atomic_load_explicit(&t->last_recv_ns, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        s1 = atomic_load_explicit(&t->seq, memory_order_relaxed);
    } while ((s0 & 1u) || s0 != s1);
}
