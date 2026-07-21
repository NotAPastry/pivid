#include "ring_user.h"

#include <string.h>

int ring_user_push(void *ring, const uint8_t *payload, size_t len) {
    /* `payload` is the full protocol packet (flags + [len] + payload); max is
     * the FLRC 511 B on-air ceiling. The ring's own 2-byte LE length prefix is
     * added below, so the on-ring record is at most 513 bytes. */
    if (len > 511) return -1;

    uint32_t *wrp  = ring_wr_ptr(ring);
    uint32_t *rdp  = ring_rd_ptr(ring);
    uint8_t  *data = ring_data(ring);

    uint32_t wr = *wrp;
    uint32_t rd = __atomic_load_n(rdp, __ATOMIC_ACQUIRE);

    uint32_t used = wr - rd;
    uint32_t free_bytes = RADIO_RING_DATA_BYTES - 1u - used;
    if (free_bytes < 2u + (uint32_t)len) return -1;          /* ring full */

    /* Write the 2-byte LE length prefix, then the payload (each may wrap). */
    uint32_t off = wr & RADIO_RING_DATA_MASK;
    data[off] = (uint8_t)(len & 0xFF);
    off = (off + 1u) & RADIO_RING_DATA_MASK;
    data[off] = (uint8_t)((len >> 8) & 0xFF);
    off = (off + 1u) & RADIO_RING_DATA_MASK;

    if (len > 0) {
        uint32_t first = RADIO_RING_DATA_BYTES - off;
        if ((size_t)first >= len) {
            memcpy(&data[off], payload, len);
        } else {
            memcpy(&data[off], payload, first);
            memcpy(&data[0], payload + first, len - first);
        }
    }

    __atomic_store_n(wrp, wr + 2u + (uint32_t)len, __ATOMIC_RELEASE);
    return 0;
}

int ring_user_pop(void *ring, uint8_t *out, size_t out_max) {
    uint32_t *wrp  = ring_wr_ptr(ring);
    uint32_t *rdp  = ring_rd_ptr(ring);
    uint8_t  *data = ring_data(ring);

    /* ACQUIRE on wr_idx; pairs with kernel smp_store_release on the producer
     * side (or the userspace producer's release on the TX direction, used by
     * tests). Sole consumer: rd_idx can be read non-atomically. */
    uint32_t wr = __atomic_load_n(wrp, __ATOMIC_ACQUIRE);
    uint32_t rd = *rdp;

    uint32_t used = wr - rd;
    if (used < 2u) return 0;                                  /* empty */

    uint32_t off = rd & RADIO_RING_DATA_MASK;
    uint32_t len = data[off];
    off = (off + 1u) & RADIO_RING_DATA_MASK;
    len |= (uint32_t)data[off] << 8;
    off = (off + 1u) & RADIO_RING_DATA_MASK;

    if (used < 2u + len) return -1;                           /* corrupt / torn */
    if ((size_t)len > out_max) return -1;                     /* caller bug */

    if (len > 0) {
        uint32_t first = RADIO_RING_DATA_BYTES - off;
        if (first >= len) {
            memcpy(out, &data[off], len);
        } else {
            memcpy(out, &data[off], first);
            memcpy(out + first, &data[0], len - first);
        }
    }

    /* Advance rd_idx with RELEASE; producer pairs with __atomic_load_n
     * ACQUIRE on the rd side (or the kernel's smp_load_acquire). */
    __atomic_store_n(rdp, rd + 2u + len, __ATOMIC_RELEASE);
    return (int)len;
}
