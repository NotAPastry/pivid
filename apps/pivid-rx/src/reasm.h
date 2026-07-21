#ifndef RX_REASM_H
#define RX_REASM_H

#include <stddef.h>
#include <stdint.h>

/* Max reassembled NAL size. H.264 IDR slices at 800 kbps/30 fps/intra=5
 * are 8-15 KB; the original 4048-byte cap (sized for a single 4-bit SEQ
 * wrap at PROTOCOL_MAX_PAYLOAD) was masking the actual encoder output
 * size while TX FIFO underflow was killing trains pre-reassembly. With
 * underflow fixed (drivers/lr2021/lr2021_tx.c padding to 255 B) every
 * IDR now reaches the reassembler intact and trips dropped_overflow if
 * it exceeds the buffer.
 *
 * 64 KB covers H.264 baseline NALs up to a 4 Mbps stream at 30 fps with
 * intra=1 -- comfortable headroom over the 800 kbps configured rate.
 * SEQ wraps fine across this length because reasm_feed matches
 * expected_seq == seq mod 16 (no ambiguity within a contiguous train). */
#define REASM_BUF_BYTES 65536
#define REASM_TIMEOUT_NS 500000000ull  /* 500 ms */

/* `payload` is reasm-owned and valid only for the duration of the call;
 * callers must copy if they need to retain it. */
typedef void (*Reasm_EmitCb)(const uint8_t *payload, size_t len, void *user);

typedef struct {
    uint8_t  buf[REASM_BUF_BYTES];
    size_t   buf_len;
    size_t   expected_total;      /* total NAL length from the START prefix (multi-frag) */
    int      in_progress;
    uint8_t  expected_seq;        /* next SEQ we want (mod 16) */
    uint64_t last_progress_ns;    /* CLOCK_MONOTONIC ns; tests may poke this */

    Reasm_EmitCb emit;
    void        *user;

    uint64_t nals_emitted;
    uint64_t dropped_seq_mismatch;
    uint64_t dropped_timeout;
    uint64_t dropped_overflow;
    uint64_t dropped_invalid;
    uint64_t dropped_len_mismatch;   /* END buf_len != START-advertised total */
} Reasm;

void reasm_init(Reasm *r, Reasm_EmitCb emit, void *user);

/* Feed one [flags][length][payload] record popped off the RX ring. */
void reasm_feed(Reasm *r, const uint8_t *record, size_t record_len);

/* Call once per main-loop iteration. Discards an in-progress NAL if no
 * progress has been made for REASM_TIMEOUT_NS. */
void reasm_tick(Reasm *r);

#endif
