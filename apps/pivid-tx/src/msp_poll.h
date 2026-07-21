#ifndef TX_MSP_POLL_H
#define TX_MSP_POLL_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "tx.h"
#include "settings.h"

/* Flight-controller link states reported in fc_link_state below. The numeric
 * codes are defined as an anonymous enum in msp_poll.c:
 *   0=FC_OK 1=FC_INIT 2=FC_TIMEOUT 3=FC_UART_ERROR.
 * FC_LINK_STATE_COUNT bounds that range so the heartbeat writer can clamp any
 * out-of-range value to an "unknown" sentinel instead of silently wrapping a
 * future-added state to 0 (which would falsely read as OK on the dashboard). */
#define FC_LINK_STATE_COUNT   4
#define FC_LINK_STATE_UNKNOWN FC_LINK_STATE_COUNT   /* sentinel: dashboard shows "unknown" */

typedef struct {
    pthread_t        thread;
    bool             thread_started;   /* true between a successful start and stop */
    int              fd;            /* UART, -1 when closed */
    TX_Dev          *dev;           /* shared, mutex-protected tx_push */
    const Settings  *settings;
    atomic_bool      stop;
    /* status snapshot for the heartbeat writer (read by main thread) */
    atomic_int       fc_link_state;
    atomic_uint      polls_ok;      /* successful poll cycles since start */
    atomic_ullong    parse_resets;
    atomic_ullong    crc_failures;
    atomic_ullong    kf_frags_tx;   /* OSD-grid fragments pushed to the radio ring */
} MSP_Poll;

/* Spawns the poller thread. Returns 0 on success. Safe no-op + returns 0 if
 * settings->msp_enabled is false (caller checks too). */
int  msp_poll_start(MSP_Poll *m, TX_Dev *dev, const Settings *s);
/* Signals stop and joins. */
void msp_poll_stop(MSP_Poll *m);
#endif
