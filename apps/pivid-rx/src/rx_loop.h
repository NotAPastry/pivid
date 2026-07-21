#ifndef RX_RX_LOOP_H
#define RX_RX_LOOP_H

#include <stdint.h>
#include <stdatomic.h>

#include "fec_rx.h"
#include "osd_grid_rx.h"
#include "output.h"
#include "reasm.h"
#include "rx.h"

typedef struct {
    RX_Dev      *dev;
    Reasm       *reasm;
    FecRx       *fec;             /* FEC-decode stage; sits between ring and reasm */
    Output_Sink *sink;            /* stored for main's heartbeat reads / swap during bind;
                                     the emit-to-sink wiring lives in main.c's reasm callback */
    RxGrid      *grid;            /* OSD-grid reassembly + injection store; TYPE=1
                                     PKT_OSD_GRID fragments are reassembled
                                     here (NOT fed to fec), TYPE=0 video records
                                     bump the grid's rx-video counter. Owned by main. */
    int          wake_fd;

    uint64_t poll_wakes;
    uint64_t records_popped;

    uint32_t drop_pct;            /* DEBUG ONLY: PIVID_RX_DROP_PCT injected loss, 0=off */
    uint64_t drop_state;          /* fractional accumulator for deterministic drop */
    uint64_t injected_drops;      /* count of records dropped by the debug knob */

    uint64_t last_stats_ms;       /* monotonic ms; rx_loop_run returns 3 when 5 s have elapsed */

    _Atomic uint64_t last_valid_rx_ns;  /* monotonic ns of the last CRC-valid
                                           record popped from the ring; read by
                                           the GPIO thread to drive the RX
                                           activity LED. 0 = none yet. */
} RX_Loop;

void rx_loop_init(RX_Loop *l, RX_Dev *dev, Reasm *r, FecRx *fec, Output_Sink *sink, int wake_fd);

/* DEBUG: deterministic fractional drop. Advances *state by pct each call and
 * drops (returns 1) each time the accumulator crosses 100. pct in 0..100.
 * Exposed for unit tests. */
int rx_loop_should_drop(uint64_t *state, uint32_t pct);

/* Drain the RX ring until empty, feeding each record into the reassembler.
 * Returns the number of records popped (>=0). Exposed for unit tests; the
 * main loop calls this on POLLIN. */
int rx_loop_drain_once(RX_Loop *l);

/* Run until wake_fd is signaled, the stats heartbeat is due, or an
 * unrecoverable error occurs.
 * Returns:
 *    3 -- 5 s elapsed since last_stats_ms (caller should log + reset)
 *    2 -- wake_fd signaled (caller's responsibility to handle bind/shutdown)
 *    1 -- unrecoverable error from /dev/radio0 (POLLERR/POLLHUP)
 *   -1 -- poll() failed with errno other than EINTR */
int rx_loop_run(RX_Loop *l);

#endif
