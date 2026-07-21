#ifndef COMMON_RING_USER_H
#define COMMON_RING_USER_H

#include <stddef.h>
#include <stdint.h>
#include "radio_ioctl.h"   /* RADIO_RING_HEADER_BYTES, RADIO_RING_DATA_BYTES, RADIO_RING_DATA_MASK */

/* Userspace view of one mmap'd ring. Field offsets MUST match the kernel
 * `struct lr2021_ring` from drivers/lr2021/lr2021_ring.h:
 *   offset  0: wr_idx (uint32, free-running)
 *   offset 64: rd_idx (uint32, free-running, on its own cacheline)
 *   offset 4096: data[RADIO_RING_DATA_BYTES]
 *
 * We do NOT cast the mmap pointer to a struct -- we use byte-offset accessors
 * so this header has zero dependencies on kernel-style types (__u32 etc).
 * The mmap'd storage was created by the kernel with __u32 wr_idx / rd_idx
 * fields at these exact offsets, so accessing them as uint32_t* from
 * userspace is well-defined (same effective type, aligned by construction). */
static inline uint32_t *ring_wr_ptr(void *ring) { return (uint32_t *)((char *)ring + 0);  }
static inline uint32_t *ring_rd_ptr(void *ring) { return (uint32_t *)((char *)ring + 64); }
static inline uint8_t  *ring_data  (void *ring) { return (uint8_t  *)ring + RADIO_RING_HEADER_BYTES; }

/* Producer: push one record `[2-byte LE len][payload]` into the ring.
 * Returns 0 on success, -1 if the ring lacks `2 + len` free bytes (caller
 * drops the packet, bumps its own "ring full" counter). Uses RELEASE on the
 * wr_idx publish; the kernel consumer matches with smp_load_acquire.
 *
 * `len` is the size of one protocol packet (flags + length + NAL payload),
 * up to 511 bytes (the FLRC 511 B on-air ceiling). The ring's own 2-byte LE
 * length prefix is added by ring_user_push; do not include it in `payload`.
 * Safe to call from a single producer thread; not safe for concurrent
 * producers (single-producer ring). */
int ring_user_push(void *ring, const uint8_t *payload, size_t len);

/* Consumer: pop one record. Returns the payload length on success, 0 if the
 * ring is empty, -1 if the next record's length prefix is corrupt (should never
 * happen on a well-formed ring -- treat as a bug). Uses ACQUIRE on the wr_idx
 * load; matches kernel smp_store_release on the producer side. Caller passes
 * `out_max` (size of `out`); record is silently truncated if longer than
 * `out_max` (should never happen with the agreed 511-byte cap, so callers
 * should pass an `out_max >= 511`).
 *
 * Used by the RX daemon in Phase 5. Provided here so both directions of the
 * ring ABI live in one file. */
int ring_user_pop(void *ring, uint8_t *out, size_t out_max);

#endif
