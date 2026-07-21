#include "msp_parse.h"
#include <string.h>

void msp_parser_init(MSP_Parser *p) {
    memset(p, 0, sizeof *p);
    p->state = MSP_WAIT_DOLLAR;
}

static int reset_err(MSP_Parser *p) {
    p->parse_resets++;
    p->state = MSP_WAIT_DOLLAR;
    p->payload_idx = 0;
    p->crc_running = 0;
    return -1;
}

int msp_parser_feed(MSP_Parser *p, uint8_t b) {
    switch (p->state) {
    case MSP_WAIT_DOLLAR:
        if (b == '$') p->state = MSP_WAIT_M;   /* a '$' re-arms from any state */
        return 0;
    case MSP_WAIT_M:
        if (b == 'M') { p->state = MSP_WAIT_DIR; return 0; }
        if (b == '$') { p->state = MSP_WAIT_M; return 0; }
        return reset_err(p);
    case MSP_WAIT_DIR:
        if (b == '>') { p->state = MSP_WAIT_SIZE; return 0; }   /* response dir */
        if (b == '$') { p->state = MSP_WAIT_M; return 0; }
        return reset_err(p);
    case MSP_WAIT_SIZE:
        p->size = b; p->crc_running = b; p->payload_idx = 0;
        p->state = MSP_WAIT_CMD;
        return 0;
    case MSP_WAIT_CMD:
        p->cmd = b; p->crc_running ^= b;
        p->state = (p->size == 0) ? MSP_READ_CRC : MSP_READ_PAYLOAD;
        return 0;
    case MSP_READ_PAYLOAD:
        /* MSPv1 is length-delimited: size+cmd are known, so store every payload
         * byte verbatim -- including 0x24 ('$') -- and let CRC validate the frame.
         * '$' is only a re-arm while hunting the preamble (WAIT_* states), not
         * mid-frame. A genuinely truncated frame self-heals via a CRC mismatch
         * below, costing at most the next frame before the following '$' re-arms;
         * on the clean wired UART (crc_failures==0) truncation never occurs. */
        p->payload[p->payload_idx++] = b; p->crc_running ^= b;
        if (p->payload_idx >= p->size) p->state = MSP_READ_CRC;
        return 0;
    case MSP_READ_CRC:
        if (b != p->crc_running) { p->crc_failures++; return reset_err(p); }
        p->state = MSP_WAIT_DOLLAR;   /* next byte re-arms; caller consumes now */
        return 1;
    }
    return reset_err(p);
}
