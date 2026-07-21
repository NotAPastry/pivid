#define _POSIX_C_SOURCE 200809L

#include "reasm.h"
#include "protocol.h"

#include <string.h>
#include <time.h>

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void reset_partial(Reasm *r) {
    r->in_progress = 0;
    r->buf_len     = 0;
}

static void emit_nal(Reasm *r, const uint8_t *p, size_t n) {
    r->nals_emitted++;
    if (r->emit) r->emit(p, n, r->user);
}

void reasm_init(Reasm *r, Reasm_EmitCb emit, void *user) {
    memset(r, 0, sizeof(*r));
    r->emit = emit;
    r->user = user;
}

void reasm_feed(Reasm *r, const uint8_t *record, size_t record_len) {
    uint8_t flags = 0;
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
    size_t  plen = 0;
    if (protocol_parse(record, record_len, PROTO_MODE_FIXED, &flags, payload, &plen) < 0) {
        r->dropped_invalid++;
        return;
    }

    const int start = (flags & PROTO_FLAG_START) != 0;
    const int end   = (flags & PROTO_FLAG_END)   != 0;
    const uint8_t seq = (uint8_t)PROTO_SEQ_FROM_FLAGS(flags);

    /* Single-packet train: emit immediately. Any in-progress is lost. */
    if (start && end) {
        if (r->in_progress) {
            r->dropped_seq_mismatch++;
            reset_partial(r);
        }
        emit_nal(r, payload, plen);
        return;
    }

    if (start) {
        if (r->in_progress) {
            r->dropped_seq_mismatch++;
        }
        /* A multi-fragment START (reached only when NOT start&&end) carries a
         * 2-byte LE total-length prefix ahead of the first chunk. */
        if (plen < 2) {
            r->dropped_invalid++;
            reset_partial(r);
            return;
        }
        size_t total = (size_t)unpack_le16(payload);
        size_t body  = plen - 2;
        if (body > REASM_BUF_BYTES) {
            r->dropped_overflow++;
            reset_partial(r);
            return;
        }
        memcpy(r->buf, payload + 2, body);
        r->buf_len          = body;
        r->expected_total   = total;
        r->in_progress      = 1;
        r->expected_seq     = (uint8_t)((seq + 1) & 0x0F);
        r->last_progress_ns = mono_ns();
        return;
    }

    /* Continuation or END: require in-progress and SEQ match. */
    if (!r->in_progress || seq != r->expected_seq) {
        if (r->in_progress) r->dropped_seq_mismatch++;
        reset_partial(r);
        return;
    }

    if (r->buf_len + plen > REASM_BUF_BYTES) {
        r->dropped_overflow++;
        reset_partial(r);
        return;
    }
    memcpy(&r->buf[r->buf_len], payload, plen);
    r->buf_len += plen;
    r->expected_seq = (uint8_t)((seq + 1) & 0x0F);
    r->last_progress_ns = mono_ns();

    if (end) {
        if (r->buf_len != r->expected_total) {
            r->dropped_len_mismatch++;
            reset_partial(r);
            return;
        }
        emit_nal(r, r->buf, r->buf_len);
        reset_partial(r);
    }
}

void reasm_tick(Reasm *r) {
    if (!r->in_progress) return;
    uint64_t now = mono_ns();
    if (now - r->last_progress_ns > REASM_TIMEOUT_NS) {
        r->dropped_timeout++;
        reset_partial(r);
    }
}
