/* tests/test_dp_client.c */
#include "dp_client.h"
#include "msp_request.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    DpClient d; dp_client_init(&d);

    uint8_t clr[] = { MSP_DP_CLEAR };
    assert(dp_client_apply(&d, clr, sizeof clr) == 0);

    /* WRITE_STRING: subcmd, row=1, col=2, attr=0, "HI" */
    uint8_t ws[] = { MSP_DP_WRITE_STRING, 1, 2, 0, 'H', 'I' };
    assert(dp_client_apply(&d, ws, sizeof ws) == 0);
    assert(d.grid.cells[1 * OSD_GRID_COLS + 2].ch == 'H');
    assert(d.grid.cells[1 * OSD_GRID_COLS + 3].ch == 'I');

    assert(d.dirty == false);
    uint8_t draw[] = { MSP_DP_DRAW_SCREEN };
    assert(dp_client_apply(&d, draw, sizeof draw) == 0);
    assert(d.dirty == true);

    uint8_t hb[] = { MSP_DP_HEARTBEAT };
    assert(dp_client_apply(&d, hb, sizeof hb) == 0);    /* recognized no-op */

    /* BF master also emits SYS(6) and FONTCHAR_WRITE(7); recognized no-ops,
     * must NOT be reported malformed (would pollute error counters). */
    uint8_t sys[] = { MSP_DP_SYS, 0, 0, 0 };
    assert(dp_client_apply(&d, sys, sizeof sys) == 0);
    uint8_t fc[] = { MSP_DP_FONTCHAR_WRITE, 0 };
    assert(dp_client_apply(&d, fc, sizeof fc) == 0);

    assert(dp_client_apply(&d, ws, 2) == -1);           /* truncated WRITE_STRING */
    assert(dp_client_apply(&d, (uint8_t[]){0}, 0) == -1); /* empty */

    /* a genuinely unknown sub-command IS malformed */
    uint8_t bogus[] = { 200 };
    assert(dp_client_apply(&d, bogus, sizeof bogus) == -1);

    /* CLEAR blanks a previously-written cell */
    uint8_t ws2[] = { MSP_DP_WRITE_STRING, 3, 5, 0, 'X' };
    assert(dp_client_apply(&d, ws2, sizeof ws2) == 0);
    assert(d.grid.cells[3 * OSD_GRID_COLS + 5].ch == 'X');
    uint8_t clr2[] = { MSP_DP_CLEAR };
    assert(dp_client_apply(&d, clr2, sizeof clr2) == 0);
    assert(d.grid.cells[3 * OSD_GRID_COLS + 5].ch == OSD_GRID_BLANK);

    /* WRITE_STRING with len==4 (zero chars) returns 0 and writes nothing */
    assert(d.grid.cells[4 * OSD_GRID_COLS + 0].ch == OSD_GRID_BLANK);
    uint8_t ws_empty[] = { MSP_DP_WRITE_STRING, 4, 0, 0 };  /* exactly 4 bytes */
    assert(dp_client_apply(&d, ws_empty, sizeof ws_empty) == 0);
    assert(d.grid.cells[4 * OSD_GRID_COLS + 0].ch == OSD_GRID_BLANK);

    /* attr propagation: BLINK attr is stored on the written cell */
    uint8_t ws_attr[] = { MSP_DP_WRITE_STRING, 5, 7, MSP_DP_ATTR_BLINK, 'A' };
    assert(dp_client_apply(&d, ws_attr, sizeof ws_attr) == 0);
    assert(d.grid.cells[5 * OSD_GRID_COLS + 7].ch   == 'A');
    assert(d.grid.cells[5 * OSD_GRID_COLS + 7].attr == MSP_DP_ATTR_BLINK);

    /* committed snapshot: a keyframe must encode the grid state AS OF the last
     * DRAW_SCREEN, not the live grid. Writes after DRAW_SCREEN must NOT leak into
     * `committed` until the next DRAW_SCREEN -- this is what stops moving elements
     * (BF erases+rewrites them across WRITE_STRINGs) from flickering. */
    DpClient e; dp_client_init(&e);
    uint8_t wa[] = { MSP_DP_WRITE_STRING, 0, 0, 0, 'A' };
    assert(dp_client_apply(&e, wa, sizeof wa) == 0);
    uint8_t drw[] = { MSP_DP_DRAW_SCREEN };
    assert(dp_client_apply(&e, drw, sizeof drw) == 0);
    assert(e.committed.cells[0].ch == 'A');             /* snapshot captured 'A' */
    uint8_t wb[] = { MSP_DP_WRITE_STRING, 0, 0, 0, 'B' };
    assert(dp_client_apply(&e, wb, sizeof wb) == 0);     /* mid-frame write */
    assert(e.grid.cells[0].ch == 'B');                  /* live grid advanced */
    assert(e.committed.cells[0].ch == 'A');             /* committed unchanged */
    assert(dp_client_apply(&e, drw, sizeof drw) == 0);  /* next frame boundary */
    assert(e.committed.cells[0].ch == 'B');             /* re-snapshotted */

    printf("test_dp_client: OK\n");
    return 0;
}
