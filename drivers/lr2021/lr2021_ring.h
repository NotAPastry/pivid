#ifndef LR2021_RING_H
#define LR2021_RING_H

#include "radio_ioctl.h"   /* RADIO_RING_BYTES, RADIO_RING_DATA_BYTES, RADIO_RING_DATA_MASK, ... */

#ifdef __KERNEL__
#  include <linux/types.h>
#  include <linux/string.h>
#else
#  include <linux/types.h>   /* available on glibc/Linux for __u8 / __u32 */
#  include <string.h>        /* memcpy */
#endif

#define LR2021_RING_MAX_RECORD 513   /* 2 length bytes + up to 511 payload bytes */

/* Layout of one ring, exactly RADIO_RING_BYTES total. The producer (which
 * may be userspace or kernel depending on the ring) updates `wr_idx`; the
 * consumer (the other side) updates `rd_idx`. `data[]` is a power-of-two
 * circular byte buffer holding [2-byte LE len][payload] records.
 *
 * Indices are free-running uint32; physical position within data[] is
 * `idx & RADIO_RING_DATA_MASK`. The data buffer being a power of two is
 * load-bearing -- it makes the physical offset continuous across the uint32
 * wraparound (2^32 mod RADIO_RING_DATA_BYTES = 0). */
struct lr2021_ring {
    /* --- header page (RADIO_RING_HEADER_BYTES, 4 KB) --- */
    __u32 wr_idx;                                           /* offset    0 */
    __u8  _pad0[60];                                        /* cacheline pad */
    __u32 rd_idx;                                           /* offset   64 */
    __u8  _pad1[RADIO_RING_HEADER_BYTES - 64 - 4];          /* fill rest of header page */
    /* --- data area (RADIO_RING_DATA_BYTES, power of two) --- */
    __u8  data[RADIO_RING_DATA_BYTES];                      /* offset 4096 */
};

/* Compile-time invariants. */
_Static_assert(sizeof(struct lr2021_ring) == RADIO_RING_BYTES,
               "struct lr2021_ring must be exactly RADIO_RING_BYTES");
_Static_assert((RADIO_RING_DATA_BYTES & RADIO_RING_DATA_MASK) == 0,
               "RADIO_RING_DATA_BYTES must be a power of two");
_Static_assert(__builtin_offsetof(struct lr2021_ring, rd_idx) == 64,
               "rd_idx must land on its own 64-byte cacheline");
_Static_assert(__builtin_offsetof(struct lr2021_ring, data) == RADIO_RING_HEADER_BYTES,
               "data[] must begin at the header-page boundary");

/* ---- Pure helpers (no concurrency primitives; safe to call from
 *      kernel and userspace, used directly by tests/test_ring.c). -------- */

/* Bytes currently occupied in the data area, given (wr, rd). */
static inline __u32 lr2021_ring_used(__u32 wr, __u32 rd) {
    return wr - rd;        /* mod-2^32 subtraction is exact for power-of-2 buffer */
}

/* Bytes currently free (reserves one byte to disambiguate empty vs full). */
static inline __u32 lr2021_ring_free(__u32 wr, __u32 rd) {
    return RADIO_RING_DATA_BYTES - 1 - lr2021_ring_used(wr, rd);
}

/* Physical byte offset within data[] for a free-running index. */
static inline __u32 lr2021_ring_offset(__u32 idx) {
    return idx & RADIO_RING_DATA_MASK;
}

/* Push one [u16-LE len][payload] record. `wr_inout` is read on entry and
 * written on exit; caller does any memory-ordering primitives (kernel wrapper
 * does smp_store_release after this returns 0). Returns 0 on success, -1 if
 * the record doesn't fit (-ENOSPC equivalent). */
static inline int lr2021_ring_push_pure(__u32 *wr_inout, __u32 rd,
                                        __u8 *data,
                                        const __u8 *payload, __u16 len) {
    __u32 wr = *wr_inout;
    if (lr2021_ring_free(wr, rd) < (__u32)(2u + len)) {
        return -1;
    }
    /* 2-byte little-endian length prefix (each byte may wrap independently). */
    data[lr2021_ring_offset(wr)] = (__u8)(len & 0xFF);
    wr++;
    data[lr2021_ring_offset(wr)] = (__u8)((len >> 8) & 0xFF);
    wr++;
    /* Payload (may wrap across the end of data[]). */
    if (len > 0) {
        __u32 off = lr2021_ring_offset(wr);
        __u32 first_part = RADIO_RING_DATA_BYTES - off;
        if (first_part >= len) {
            memcpy(&data[off], payload, len);
        } else {
            memcpy(&data[off], payload, first_part);
            memcpy(&data[0], payload + first_part, (__u32)len - first_part);
        }
        wr += len;
    }
    *wr_inout = wr;
    return 0;
}

/* Pop one [u16-LE len][payload] record. `rd_inout` advances on success;
 * caller does the smp_store_release. Returns 1 if a record was popped, 0 if
 * empty, -1 if the ring is corrupt (header without matching payload). */
static inline int lr2021_ring_pop_pure(__u32 wr, __u32 *rd_inout,
                                       const __u8 *data,
                                       __u8 *out, __u16 *out_len) {
    __u32 rd = *rd_inout;
    __u32 used = lr2021_ring_used(wr, rd);
    if (used < 2) {
        return 0;   /* not even a length prefix -> empty */
    }
    __u16 len = (__u16)data[lr2021_ring_offset(rd)];
    len = (__u16)(len | ((__u16)data[lr2021_ring_offset(rd + 1)] << 8));
    if (used < (__u32)(2u + len)) {
        return -1;  /* torn / corrupt */
    }
    /* Trust-boundary clamp: the TX ring data[] is mmap'd WRITABLE to userspace,
     * which produces this length prefix. A valid record's payload is at most
     * LR2021_RING_MAX_RECORD-2 bytes (the size `out` -- dev->tx_refill_buf -- is
     * allocated for). Reject anything larger as corrupt BEFORE the memcpy below,
     * so a malformed/hostile len can never overflow the destination buffer. */
    if (len > (__u16)(LR2021_RING_MAX_RECORD - 2)) {
        return -1;  /* len exceeds any valid record / the consumer buffer */
    }
    rd += 2;
    if (len > 0) {
        __u32 off = lr2021_ring_offset(rd);
        __u32 first_part = RADIO_RING_DATA_BYTES - off;
        if (first_part >= len) {
            memcpy(out, &data[off], len);
        } else {
            memcpy(out, &data[off], first_part);
            memcpy(out + first_part, &data[0], (__u32)len - first_part);
        }
        rd += len;
    }
    *rd_inout = rd;
    *out_len = len;
    return 1;
}

/* ---- Kernel API ------------------------------------------------------- */
#ifdef __KERNEL__
struct LR2021_Device;

int  lr2021_ring_init(struct LR2021_Device *dev);
void lr2021_ring_cleanup(struct LR2021_Device *dev);

/* TX path: kernel consumes a record from the TX ring (userspace fills it).
 * Returns 1 if a record was popped, 0 if empty, <0 on corruption. */
int  lr2021_ring_tx_pop(struct LR2021_Device *dev, __u8 *out, __u16 *out_len);

/* Lock-free diagnostic peek: nonzero iff the TX ring holds at least one byte. */
bool lr2021_ring_tx_nonempty(struct LR2021_Device *dev);

/* RX path: kernel pushes a record into the RX ring (userspace reads it).
 * Returns 0 on success, -ENOSPC if the ring is full. */
int  lr2021_ring_rx_push(struct LR2021_Device *dev, const __u8 *payload, __u16 len);
#endif /* __KERNEL__ */

#endif /* LR2021_RING_H */
