#include "msp_request.h"

size_t msp_request_build_payload(uint8_t *out, uint8_t cmd, const uint8_t *body, size_t n) {
    if (n > 255) return 0;
    out[0] = '$'; out[1] = 'M'; out[2] = '<';
    out[3] = (uint8_t)n;
    out[4] = cmd;
    uint8_t crc = (uint8_t)n ^ cmd;
    for (size_t i = 0; i < n; i++) { out[5 + i] = body[i]; crc ^= body[i]; }
    out[5 + n] = crc;
    return 6 + n;
}
