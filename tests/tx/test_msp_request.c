#include "msp_request.h"
#include <stdio.h>

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } \
} while (0)

int main(void) {
    /* msp_request_build_payload: a DisplayPort HEARTBEAT request. */
    {
        uint8_t out[7];
        uint8_t body[1] = { MSP_DP_HEARTBEAT };  /* {0} */
        size_t pn = msp_request_build_payload(out, MSP_DISPLAYPORT, body, 1);
        EXPECT(pn == 7, "build_payload heartbeat length 7");
        EXPECT(out[0]=='$' && out[1]=='M' && out[2]=='<', "build_payload preamble $M<");
        EXPECT(out[3] == 1, "build_payload size 1");
        EXPECT(out[4] == MSP_DISPLAYPORT, "build_payload cmd MSP_DISPLAYPORT (182)");
        EXPECT(out[5] == MSP_DP_HEARTBEAT, "build_payload body byte 0");
        EXPECT(out[6] == (uint8_t)(1 ^ 182 ^ 0), "build_payload crc = size^cmd^body");
    }
    /* msp_request_build_payload: n > 255 returns 0 (MSPv1 size byte is u8). */
    {
        uint8_t big_out[262];
        uint8_t big_body[256] = {0};
        size_t pn = msp_request_build_payload(big_out, MSP_DISPLAYPORT, big_body, 256);
        EXPECT(pn == 0, "build_payload n>255 returns 0");
    }

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("OK\n");
    return 0;
}
