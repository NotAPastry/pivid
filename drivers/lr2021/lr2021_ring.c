#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/atomic.h>     /* smp_load_acquire / smp_store_release */
#include "lr2021.h"
#include "lr2021_ring.h"

/* Allocate both ring buffers. Uses kmalloc so the kernel direct-map
 * mapping in lr2021_mmap (virt_to_phys + remap_pfn_range) is valid.
 * The struct is 36864 bytes -- well within kmalloc's per-allocation limit
 * (typically ~4 MB) and the slab will return a kernel direct-mapped
 * pointer. GFP_KERNEL is OK at probe time. kzalloc to zero wr/rd indices. */
int lr2021_ring_init(LR2021_Device *dev) {
    dev->tx_ring = kzalloc(sizeof(struct lr2021_ring), GFP_KERNEL);
    if (!dev->tx_ring) {
        return -ENOMEM;
    }
    dev->rx_ring = kzalloc(sizeof(struct lr2021_ring), GFP_KERNEL);
    if (!dev->rx_ring) {
        kfree(dev->tx_ring);
        dev->tx_ring = NULL;
        return -ENOMEM;
    }
    return 0;
}

void lr2021_ring_cleanup(LR2021_Device *dev) {
    kfree(dev->tx_ring);
    kfree(dev->rx_ring);
    dev->tx_ring = NULL;
    dev->rx_ring = NULL;
}

/* TX consumer (kernel). Reads from the TX ring filled by userspace.
 *
 *   - load wr with acquire   (peer index; producer = userspace)
 *   - call pure pop helper   (advances rd)
 *   - store rd with release  (publishes our progress to the producer)
 *
 * The acquire/release pair ensures: if the producer wrote a record and
 * published the new wr_idx, then when we observe that new wr_idx we also
 * see the record bytes that were written before it. */
int lr2021_ring_tx_pop(LR2021_Device *dev, __u8 *out, __u16 *out_len) {
    struct lr2021_ring *ring = dev->tx_ring;
    __u32 wr = smp_load_acquire(&ring->wr_idx);
    __u32 rd = ring->rd_idx;          /* local index; no barrier needed */

    int rc = lr2021_ring_pop_pure(wr, &rd, ring->data, out, out_len);
    if (rc == 1) {
        smp_store_release(&ring->rd_idx, rd);
    }
    return rc;
}

/* Lock-free peek: is there at least one byte of record in the TX ring? Uses the
 * same acquire load as the pop so the answer is coherent with the producer's
 * wr_idx publish. Diagnostic only. */
bool lr2021_ring_tx_nonempty(LR2021_Device *dev) {
    struct lr2021_ring *ring = dev->tx_ring;
    __u32 wr = smp_load_acquire(&ring->wr_idx);
    __u32 rd = READ_ONCE(ring->rd_idx);
    return lr2021_ring_used(wr, rd) > 0;
}

/* RX producer (kernel). Pushes a record into the RX ring read by userspace.
 *
 *   - load rd with acquire   (peer index; consumer = userspace)
 *   - call pure push helper  (writes data, advances wr)
 *   - store wr with release  (publishes the new record to the consumer) */
int lr2021_ring_rx_push(LR2021_Device *dev, const __u8 *payload, __u16 len) {
    struct lr2021_ring *ring = dev->rx_ring;
    __u32 rd = smp_load_acquire(&ring->rd_idx);
    __u32 wr = ring->wr_idx;

    int rc = lr2021_ring_push_pure(&wr, rd, ring->data, payload, len);
    if (rc == 0) {
        smp_store_release(&ring->wr_idx, wr);
        return 0;
    }
    return -ENOSPC;
}
