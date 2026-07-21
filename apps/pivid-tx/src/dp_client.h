/* apps/pivid-tx/src/dp_client.h -- fold MSP DisplayPort sub-commands into a
 * 30x16 shadow grid. Pure (no UART): caller feeds decoded MSP payloads. */
#ifndef TX_DP_CLIENT_H
#define TX_DP_CLIENT_H
#include "osd_grid.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    osd_grid_t grid;       /* live shadow grid: mutated continuously as BF writes */
    osd_grid_t committed;   /* last COMPLETE frame: snapshot of `grid` taken at the
                            * most recent DRAW_SCREEN. The caller transmits THIS, not
                            * `grid`, so a keyframe is never encoded mid-update (BF
                            * erases+rewrites moving elements like the horizon across
                            * several WRITE_STRINGs; sampling `grid` between them made
                            * those cells flicker -- HW-observed 2026-06-20). */
    bool       dirty;      /* set true on DRAW_SCREEN; caller must read-and-clear
                            * after transmitting. dp_client_apply never clears it. */
} DpClient;

void dp_client_init(DpClient *d);
/* Apply one MSP_DISPLAYPORT payload (sub-command byte + args).
 * Returns 0 if recognized/applied, -1 if malformed/too short.
 *
 * Return value does NOT mean "frame ready": a frame becomes ready only when
 * DRAW_SCREEN is received, which sets d->dirty = true. The caller must
 * read-and-clear d->dirty after it has consumed (transmitted) the frame;
 * dp_client_apply never clears it. */
int  dp_client_apply(DpClient *d, const uint8_t *payload, size_t len);
#endif
