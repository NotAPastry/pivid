/* apps/pivid-caster/src/control.h -- pivid-caster control protocol.
 *
 * Two tiny, versioned, little-endian datagrams sent by a player to the caster's
 * control socket: SUBSCRIBE (also the keepalive, re-sent ~every 2 s) and
 * UNSUBSCRIBE. The caster owns this protocol (it is the server); the player
 * mirrors the constants or reads the control port from the mDNS TXT
 * record. Field-by-field LE packing -- never a struct memcpy.
 *
 * Wire layout (little-endian):
 *   off  type  field
 *    0   u32   magic = CASTER_CTRL_MAGIC
 *    4   u8    version = CASTER_CTRL_VERSION
 *    5   u8    type    = CASTER_MSG_SUBSCRIBE | CASTER_MSG_UNSUBSCRIBE
 *   --- SUBSCRIBE only: ---
 *    6   u16   video_port   (where the player wants video unicast)
 *    8   u16   telem_port   (where the player wants telemetry unicast)
 * SUBSCRIBE is 10 bytes; UNSUBSCRIBE is the 6-byte header only.
 */
#ifndef CASTER_CONTROL_H
#define CASTER_CONTROL_H
#include <stddef.h>
#include <stdint.h>

/* Default control port the caster binds and advertises (TXT). The player sends
 * SUBSCRIBE here; it learns this from mDNS, or defaults to it under --caster. */
#define CASTER_CTRL_PORT          5700

#define CASTER_CTRL_MAGIC         0x50564354u  /* 'PVCT'; LE wire bytes 54 43 56 50 */
#define CASTER_CTRL_VERSION       1u
#define CASTER_CTRL_HEADER_LEN    6u
#define CASTER_MSG_SUBSCRIBE      1u
#define CASTER_MSG_UNSUBSCRIBE    2u
#define CASTER_CTRL_SUBSCRIBE_LEN   10u
#define CASTER_CTRL_UNSUBSCRIBE_LEN 6u

typedef struct {
    uint8_t  type;          /* CASTER_MSG_SUBSCRIBE | CASTER_MSG_UNSUBSCRIBE */
    uint16_t video_port;    /* valid only for SUBSCRIBE */
    uint16_t telem_port;    /* valid only for SUBSCRIBE */
} CasterCtrlMsg;

/* Serialize a SUBSCRIBE into `buf` (capacity `cap`). Returns bytes written
 * (CASTER_CTRL_SUBSCRIBE_LEN) or 0 if cap too small. */
size_t caster_ctrl_build_subscribe(uint8_t *buf, size_t cap,
                                   uint16_t video_port, uint16_t telem_port);

/* Serialize an UNSUBSCRIBE into `buf`. Returns bytes written
 * (CASTER_CTRL_UNSUBSCRIBE_LEN) or 0 if cap too small. */
size_t caster_ctrl_build_unsubscribe(uint8_t *buf, size_t cap);

/* Parse `len` bytes from `buf` into `*out`. Returns 0 on success, -1 on a
 * magic/version/type mismatch or a too-short packet for the declared type.
 * A SUBSCRIBE LONGER than 10 bytes is accepted (forward compat: trailing bytes
 * ignored); UNSUBSCRIBE ignores any payload past the header. */
int caster_ctrl_parse(const uint8_t *buf, size_t len, CasterCtrlMsg *out);

#endif
