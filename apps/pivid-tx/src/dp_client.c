/* apps/pivid-tx/src/dp_client.c */
#include "dp_client.h"
#include "msp_request.h"

void dp_client_init(DpClient *d) {
    osd_grid_clear(&d->grid);
    osd_grid_clear(&d->committed);
    d->dirty = false;
}

int dp_client_apply(DpClient *d, const uint8_t *payload, size_t len) {
    if (len < 1) return -1;
    uint8_t sub = payload[0];
    switch (sub) {
    case MSP_DP_HEARTBEAT:
    case MSP_DP_RELEASE:
    case MSP_DP_SET_OPTIONS:
    case MSP_DP_SYS:            /* BF system element: recognized, not rendered in v1 */
    case MSP_DP_FONTCHAR_WRITE: /* BF font upload: recognized, ignored (we use a PNG atlas) */
        return 0;                       /* recognized, no grid effect */
    case MSP_DP_CLEAR:
        osd_grid_clear(&d->grid);
        return 0;
    case MSP_DP_WRITE_STRING: {
        if (len < 4) return -1;         /* subcmd,row,col,attr at minimum */
        uint8_t row = payload[1], col = payload[2], attr = payload[3];
        osd_grid_write_string(&d->grid, row, col, attr,
                              &payload[4], len - 4);
        return 0;
    }
    case MSP_DP_DRAW_SCREEN:
        d->committed = d->grid;         /* snapshot the now-complete frame */
        d->dirty = true;                /* a full frame has been drawn */
        return 0;
    default:
        return -1;
    }
}
