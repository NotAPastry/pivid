#ifndef TX_MSP_REQUEST_H
#define TX_MSP_REQUEST_H
#include <stdint.h>
#include <stddef.h>

/* MSP DisplayPort. Command id confirmed against Betaflight src/main/msp/
 * msp_protocol.h: "#define MSP_DISPLAYPORT 182". */
#define MSP_DISPLAYPORT          182

/* MSP_DISPLAYPORT sub-commands (first payload byte). Confirmed against BF
 * src/main/io/displayport_msp.h displayPortMspCommand_e (master, 2026-06). The
 * macro names below keep the plan's spelling; the BF canonical name is noted
 * where it differs. */
#define MSP_DP_HEARTBEAT          0
#define MSP_DP_RELEASE            1
#define MSP_DP_CLEAR              2   /* BF: MSP_DP_CLEAR_SCREEN */
#define MSP_DP_WRITE_STRING       3   /* row, col, attr, chars... */
#define MSP_DP_DRAW_SCREEN        4
#define MSP_DP_SET_OPTIONS        5   /* BF: MSP_DP_OPTIONS (reserved; unused by BF) */
#define MSP_DP_SYS                6   /* BF: MSP_DP_SYS — draw system element at coords */
#define MSP_DP_FONTCHAR_WRITE     7   /* BF: font write over MSP (USE_MSP_DISPLAYPORT_FONT) */

/* WRITE_STRING attribute byte layout, confirmed against BF
 * displayPortMspAttribute_e (src/main/io/displayport_msp.h). Used by the Phase 4
 * player renderer:
 *   bits 0-1 (0x03) DISPLAYPORT_MSP_ATTR_FONT    -> font bank select (0..3, 256 chars each)
 *   bit  6   (0x40) DISPLAYPORT_MSP_ATTR_BLINK   -> device-local blink
 *   bit  7   (0x80) DISPLAYPORT_MSP_ATTR_VERSION -> format indicator, MUST be 0 for V1/V2
 * NOTE: BF has NO "invert" attribute bit. Bit 7 is the version indicator, so the
 * OSD_ATTR_INVERT=0x80 is not a real BF attribute -- the renderer must drop
 * invert or repurpose this bit deliberately. The codec treats attr as opaque, so
 * this only matters at render time. */
#define MSP_DP_ATTR_FONT_MASK    0x03u
#define MSP_DP_ATTR_BLINK        0x40u
#define MSP_DP_ATTR_VERSION      0x80u

/* Build an MSPv1 request "$M<<size><cmd><body...><crc>" into out. out must hold
 * at least 6+n bytes. Returns total bytes written (6+n), or 0 if n>255 (MSPv1
 * size byte is u8). crc = xor(size, cmd, body[0..n)). */
size_t msp_request_build_payload(uint8_t *out, uint8_t cmd, const uint8_t *body, size_t n);
#endif
