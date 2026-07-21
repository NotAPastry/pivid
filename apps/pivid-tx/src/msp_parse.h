#ifndef TX_MSP_PARSE_H
#define TX_MSP_PARSE_H
#include <stdint.h>
#include <stddef.h>

#define MSP_MAX_PAYLOAD 255   /* MSPv1 size byte is u8 */

typedef enum {
    MSP_WAIT_DOLLAR = 0, MSP_WAIT_M, MSP_WAIT_DIR,
    MSP_WAIT_SIZE, MSP_WAIT_CMD, MSP_READ_PAYLOAD, MSP_READ_CRC,
} MSP_ParseState;

typedef struct {
    MSP_ParseState state;
    uint8_t  size, cmd;
    uint8_t  payload[MSP_MAX_PAYLOAD];
    size_t   payload_idx;
    uint8_t  crc_running;       /* xor(size, cmd, payload...) */
    uint64_t parse_resets;      /* diag: abnormal re-syncs -- bad preamble/dir or
                                   bad CRC. Superset that INCLUDES crc_failures;
                                   do not sum the two. (Length-delimited payloads
                                   store 0x24 verbatim, so a payload '$' is not a
                                   reset.) */
    uint64_t crc_failures;      /* diag: CRC mismatches (also counted in parse_resets) */
} MSP_Parser;

void msp_parser_init(MSP_Parser *p);
/* Feed one byte. Returns 1 if a complete CRC-valid frame is in (cmd,payload,size),
 * 0 if more bytes needed, -1 if the parser reset on a malformed frame. After a 1,
 * consume the frame before the next feed (the next byte re-arms). */
int  msp_parser_feed(MSP_Parser *p, uint8_t b);
#endif
