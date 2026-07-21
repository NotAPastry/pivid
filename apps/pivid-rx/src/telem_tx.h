/* apps/pivid-rx/src/telem_tx.h -- background thread that serializes the RX
 * OSD grid (common/osd_grid wire) and sends it to the co-located caster
 * on the loopback telemetry port at a fixed cadence. */
#ifndef RX_TELEM_TX_H
#define RX_TELEM_TX_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include "osd_grid_rx.h"

typedef struct {
    pthread_t      th;
    int            fd;        /* connected UDP socket -> loopback telem port */
    RxGrid        *grid;      /* not owned */
    _Atomic int    stop;
    int            started;
    uint64_t       sends_ok, sends_failed;
} TelemTx;

/* Open the loopback socket and start the sender thread. Returns 0 on success,
 * -1 on socket/thread failure (nothing left running on failure). */
int  telem_tx_start(TelemTx *t, RxGrid *grid);

/* Signal the thread to stop, join it, and close the socket. Safe if start
 * failed/was never called (no-op when !started). */
void telem_tx_stop(TelemTx *t);

#endif
