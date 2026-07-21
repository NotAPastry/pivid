/* apps/pivid-player/src/telem_recv.h -- OSD-grid UDP recv thread. Binds
 * 0.0.0.0:telem_port, recv's datagrams, osd_grid_wire_deserialize's them, and
 * publishes the grid to a TelemSlot. */
#ifndef PLAYER_TELEM_RECV_H
#define PLAYER_TELEM_RECV_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include "telem_slot.h"

typedef struct {
    uint16_t     telem_port;
    TelemSlot   *slot;
    int          fd;
    atomic_bool  stop;
    pthread_t    thread;
} TelemRecv;

/* Bind 0.0.0.0:telem_port and spawn the recv thread. Pass telem_port == 0 to
 * let the OS assign an ephemeral port; on success r->telem_port holds the
 * actually-bound port (advertise it to the caster). Returns 0 / -1. */
int  telem_recv_start(TelemRecv *r, uint16_t telem_port, TelemSlot *slot);
void telem_recv_stop(TelemRecv *r);

#endif
