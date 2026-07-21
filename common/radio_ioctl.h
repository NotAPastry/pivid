#ifndef COMMON_RADIO_IOCTL_H
#define COMMON_RADIO_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>      /* __u32, __s32, __s8 -- UAPI-canonical widths */

#define RADIO_IOC_MAGIC 'r'

/* Modes */
enum {
    RADIO_MODE_TX = 0,
    RADIO_MODE_RX = 1,
    RADIO_MODE_BIND_TRANSMIT = 2,
    RADIO_MODE_BIND_LISTEN = 3,
};

/* ioctl commands.
 * Types use kernel-canonical __u32/__s32/__s8 (from <linux/types.h>) rather
 * than POSIX uint32_t/int8_t -- the _IOC_SIZE bits baked into each macro
 * are derived from sizeof(type), so the ABI must lock to a known width
 * regardless of host-side stdint typedefs. */
#define RADIO_IOCTL_TX_ENQUEUE        _IO(RADIO_IOC_MAGIC, 1)
#define RADIO_IOCTL_SET_SYNC_WORD     _IOW(RADIO_IOC_MAGIC, 2, __u32)
#define RADIO_IOCTL_SET_MODE          _IOW(RADIO_IOC_MAGIC, 3, __s32)
#define RADIO_IOCTL_GET_RSSI          _IOR(RADIO_IOC_MAGIC, 4, __s8)
#define RADIO_IOCTL_RESET             _IO(RADIO_IOC_MAGIC, 5)
/* Measure-only RSSI calibration quality: RSSI measured at a forced LOW vs HIGH
 * AGC gain step, |dB|. A good (input-referred) cal reads the same at both (<~8
 * dB); a bad cal -- the POR cal corrupted by incoming RF during boot -- diverges
 * ~15 dB. NO reset, NO mode change beyond a transient gain probe -- safe on a
 * live RX. pivid-rx reloads the module at startup until this probe passes.
 * Returns the dB value via __s32 (0 if no packets / no signal -> treat as good). */
#define RADIO_IOCTL_GET_CAL_DIFF      _IOR(RADIO_IOC_MAGIC, 8, __s32)

/* ---- statistics snapshot (RADIO_IOCTL_GET_STATS) -----------------------
 * Driver-maintained counters, copied out in one shot. Cumulative since
 * driver load (consumers compute their own deltas). Shared kernel<->user
 * ABI: fixed-width types + explicit padding + version/size guard. Bump
 * RADIO_STATS_VERSION on ANY layout change so old userspace fails closed.
 *
 * Counter semantics (see lr2021_irq.c):
 *   errors_irq        - IRQ-path hard errors (get-length / bogus-length /
 *                       fifo-read / irq-status-read failures). Does NOT
 *                       include CRC drops or ring-full drops anymore.
 *   rx_ring_full_drops- RX packets dropped because the kernel rx_ring was
 *                       full = userspace too slow (host overrun).
 *   crc_drops         - RX_DONE packets dropped because CRC failed (RF).
 *   crc/sync/len/tmout- per-IRQ FLRC error flags seen (RF link quality).
 *   rx_silent_ms      - ms since the last DIO9 IRQ (link-liveness). The
 *                       sentinel 0xFFFFFFFF means "no packet received yet".
 */
#define RADIO_STATS_VERSION 1u

struct radio_stats {
    __u32 version;      /* = RADIO_STATS_VERSION                          */
    __u32 size;         /* = sizeof(struct radio_stats); fwd-compat guard */
    __u64 packets_tx;
    __u64 packets_rx;
    __u64 bytes_tx;
    __u64 bytes_rx;
    __u64 errors_spi;
    __u64 errors_irq;
    __u64 rx_ring_full_drops;
    __u32 crc;
    __u32 sync;
    __u32 len;
    __u32 tmout;
    __u32 crc_drops;
    __u32 rx_fifo_overflow;
    __u32 tx_fifo_underflow;
    __u32 wedge_soft;
    __u32 wedge_hard;
    __u32 tx_timeouts_consecutive;
    __u32 rx_silent_ms;
    __u32 _pad;         /* explicit; keeps the struct 8-byte aligned      */
};

#define RADIO_IOCTL_GET_STATS _IOR(RADIO_IOC_MAGIC, 6, struct radio_stats)

/* ---- FLRC runtime config (RADIO_IOCTL_SET_FLRC_CONFIG) -----------------
 * Reconfigures the FLRC packet/modulation params at runtime so Phase-3
 * experiments sweep options without recompile. Field codes are the RAW
 * datasheet values from SetFlrcModulationParams (18.4.1, table 18-4) and
 * SetFlrcPacketParams (18.4.2, table 18-5), NOT human units, so the full
 * chip range is reachable:
 *   mode        : 0 = dynamic (pkt_fmt=0, on-air length header)
 *                 1 = fixed   (pkt_fmt=1, padded payload)
 *   pld_len     : 6..511. Fixed mode: every TX padded to this; the chip
 *                 pulls exactly pld_len bytes. Dynamic mode: the RX
 *                 configured MAX (legit large packets <= this aren't
 *                 LenError-rejected); per-packet length set by the TX path.
 *   cr          : SetFlrcModulationParams byte1 bits[7:4] coding-rate code.
 *                 1 = 3/4 (production). 0..? per table 18-4.
 *   crc_width   : SetFlrcPacketParams byte1 bits[1:0] crc field code
 *                 (0=off,1=16-bit,2=24-bit,3=32-bit per table 18-5).
 *   preamble_len: SetFlrcPacketParams byte0 bits[5:2] agc_pbl_len code.
 *   sync_len    : SetFlrcPacketParams byte0 bits[1:0] sync_len code.
 *   tx_low_threshold: FIFO-low watermark (bytes) for the >256 B TX refill
 *                 IRQ (ConfigFifoIrq). 0 = chip default; sized in Plan B6.
 * sync_tx / sync_match are fixed at the init values (slot 1 / MATCH_1) and
 * not exposed here; the sync WORD is set via RADIO_IOCTL_SET_SYNC_WORD.
 * Pulse shape stays at the init value (Gaussian BT 1.0).
 * A config change re-applies to the chip immediately; the link runs ONE
 * mode end-to-end (TX and RX must be configured to match out-of-band). */
struct radio_flrc_config {
    __u8  mode;              /* 0 = dynamic, 1 = fixed                    */
    __u8  cr;                /* coding-rate field code (table 18-4)       */
    __u8  crc_width;         /* crc field code 0..3 (table 18-5)          */
    __u8  preamble_len;      /* agc_pbl_len field code 0..15              */
    __u8  sync_len;          /* sync_len field code 0..3                  */
    __u8  _pad;              /* keep pld_len 2-byte aligned               */
    __u16 pld_len;           /* 6..511                                    */
    __u16 tx_low_threshold;  /* TX FIFO-low watermark in bytes           */
};

#define RADIO_IOCTL_SET_FLRC_CONFIG _IOW(RADIO_IOC_MAGIC, 7, struct radio_flrc_config)

#ifdef FLRC_PROBE
/* Debug-only (FLRC_PROBE build): trigger one dynamic-mode FLRC TX of a
 * given payload length. Resolves the per-packet dynamic-length mechanism
 * (probe-gate diagnostics). Not an ABI commitment. */
struct radio_probe_dyn_tx { __u16 len; };
#define RADIO_IOCTL_PROBE_DYN_TX _IOW(RADIO_IOC_MAGIC, 0x40, struct radio_probe_dyn_tx)
#endif

/* mmap layout.
 * The driver exposes two ring buffers (TX from userspace, RX from kernel).
 * Userspace selects which one to map by passing the matching `offset` to
 * mmap(); the kernel sees it as `vma->vm_pgoff` and dispatches accordingly.
 *
 * Each ring is one allocation of RADIO_RING_BYTES, laid out as:
 *
 *   offset 0      : header page (4 KB)
 *     [ 0..  4)   wr_idx     (free-running uint32, mod 2^32)
 *     [ 4.. 64)   _pad0      (cacheline pad to 64 bytes)
 *     [64.. 68)   rd_idx     (free-running uint32, mod 2^32)
 *     [68..128)   _pad1      (cacheline pad)
 *     [128..4096) _reserved  (future use; sized so data starts at page 1)
 *   offset 4096   : data area, RADIO_RING_DATA_BYTES (power of 2)
 *
 * Why power-of-two data: indices are free-running 32-bit, physical offset
 * is computed via `idx & RADIO_RING_DATA_MASK`. 2^32 is a multiple of
 * RADIO_RING_DATA_BYTES so the physical offset stays continuous when the
 * index overflows. With a non-power-of-two data area, the wrap from
 * (2^32-1) to 0 creates a discontinuous jump in the physical offset and
 * corrupts the ring -- see https://en.wikipedia.org/wiki/Circular_buffer
 * "Optimized POSIX implementation" for the canonical pattern.
 *
 * Why a full page for the header: keeps the data area page-aligned within
 * the allocation, lets userspace mmap with simple page-aligned offsets.
 *
 * Example (TX daemon):
 *   void *tx = mmap(NULL, RADIO_RING_BYTES, PROT_READ|PROT_WRITE,
 *                   MAP_SHARED, fd, RADIO_MMAP_TX_OFFSET);
 *
 * Example (RX daemon):
 *   void *rx = mmap(NULL, RADIO_RING_BYTES, PROT_READ|PROT_WRITE,
 *                   MAP_SHARED, fd, RADIO_MMAP_RX_OFFSET);
 *
 * RX needs PROT_WRITE because the consumer (userspace) advances rd_idx with
 * an atomic release store. The data bytes are read-only from userspace by
 * convention, not by kernel enforcement.
 */
#define RADIO_RING_DATA_KB     32
#define RADIO_RING_DATA_BYTES  (RADIO_RING_DATA_KB * 1024)          /* 32768 -- power of 2 */
#define RADIO_RING_DATA_MASK   (RADIO_RING_DATA_BYTES - 1)          /* 0x7FFF */
#define RADIO_RING_HEADER_BYTES 4096                                /* one 4K page */
#define RADIO_RING_BYTES       (RADIO_RING_HEADER_BYTES + RADIO_RING_DATA_BYTES)
                                                                    /* 36864 = 9 pages */
#define RADIO_MMAP_TX_OFFSET   0
#define RADIO_MMAP_RX_OFFSET   RADIO_RING_BYTES

#endif
