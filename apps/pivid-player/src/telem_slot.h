/* apps/pivid-player/src/telem_slot.h -- receive-side seqlock snapshot slot,
 * mirroring rx's writer seqlock. Single writer (telem thread), lock-free reader
 * (render thread). Also tracks the local arrival time of the last datagram for
 * the spec-§5 "no datagram in >1s" staleness test. */
#ifndef PLAYER_TELEM_SLOT_H
#define PLAYER_TELEM_SLOT_H
#include <stdatomic.h>
#include <stdint.h>
#include "osd_grid.h"   /* osd_grid_t */

typedef struct {
    atomic_uint                  seq;          /* even=stable, odd=writing */
    osd_grid_t                   grid;         /* guarded by seq */
    _Atomic int64_t              last_recv_ns; /* arrival of last datagram (0=none) */
} TelemSlot;

void telem_slot_init(TelemSlot *t);

/* Writer: publish a freshly-deserialized OSD grid + its local arrival time. */
void telem_slot_publish(TelemSlot *t, const osd_grid_t *grid, int64_t recv_ns);

/* Reader: lock-free copy of the latest grid; returns the last datagram
 * arrival time via *out_recv_ns (0 if none received yet). */
void telem_slot_read(const TelemSlot *t, osd_grid_t *out,
                     int64_t *out_recv_ns);

#endif
