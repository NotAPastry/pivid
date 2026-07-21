/* Host-side test stub: replaces apps/pivid-tx/src/tx.c so test_encoder can
 * link without pulling in ioctl/mmap dependencies. Records every push so
 * tests can assert on the chunking, flags, and SEQ behaviour. */

#include "../apps/pivid-tx/src/tx.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MOCK_MAX_CALLS 4096

typedef struct {
    uint8_t  bytes[512];   /* 511 B FLRC ceiling (packed FEC pairs reach 505 B) */
    size_t   len;
} Mock_Call;

Mock_Call mock_calls[MOCK_MAX_CALLS];
size_t    mock_call_count    = 0;   /* successful pushes  */
size_t    mock_attempt_count = 0;   /* total attempts including fails */

/* The test sets this to make tx_push return -1 ("ring full") on the Nth
 * attempt (1-indexed) and every multiple thereafter. 0 disables. */
int mock_fail_every_nth = 0;

void mock_reset(void) {
    mock_call_count = 0;
    mock_attempt_count = 0;
    memset(mock_calls, 0, sizeof mock_calls);
    mock_fail_every_nth = 0;
}

int tx_open(TX_Dev *dev, uint32_t initial_sync_word) {
    (void)initial_sync_word;
    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;
    return 0;
}

int tx_push_try(TX_Dev *dev, const uint8_t *bytes, size_t len) {
    if (mock_call_count >= MOCK_MAX_CALLS) {
        fprintf(stderr, "mock_tx: MOCK_MAX_CALLS exceeded\n");
        return -1;
    }
    /* Inject ring-full on every Nth ATTEMPT (1-indexed). Must use an
     * attempt counter, not the success counter, otherwise a failure
     * leaves the next iteration stuck at the same modulus. */
    mock_attempt_count++;
    if (mock_fail_every_nth > 0 &&
        (mock_attempt_count % (size_t)mock_fail_every_nth) == 0) {
        /* Do NOT record the call -- match the contract that on -1 the
         * payload is NOT enqueued. try never counts dropped_full. */
        return -1;
    }
    if (len > sizeof mock_calls[0].bytes) {
        fprintf(stderr, "mock_tx: oversize record %zu\n", len);
        return -1;
    }
    memcpy(mock_calls[mock_call_count].bytes, bytes, len);
    mock_calls[mock_call_count].len = len;
    mock_call_count++;
    dev->packets_sent++;
    dev->bytes_sent += len;
    return 0;
}

/* Same semantics as production tx_push: a full ring counts dropped_full. */
int tx_push(TX_Dev *dev, const uint8_t *bytes, size_t len) {
    int rc = tx_push_try(dev, bytes, len);
    if (rc == -1) dev->dropped_full++;
    return rc;
}

int tx_set_mode(TX_Dev *dev, int mode)         { (void)dev; (void)mode; return 0; }
int tx_set_sync_word(TX_Dev *dev, uint32_t sw) { (void)dev; (void)sw;   return 0; }
void tx_close(TX_Dev *dev)                     { (void)dev; }
