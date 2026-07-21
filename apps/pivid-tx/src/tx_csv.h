#ifndef TX_TX_CSV_H
#define TX_TX_CSV_H

#include <stdio.h>

/* Shared single-file CSV sink for the TX-side instrumentation (tx_sample +
 * aqm). The first acquirer opens `path` ("w", truncating) and writes the
 * shared header; subsequent acquirers of the SAME path get the same FILE* and
 * the refcount rises. release() drops the refcount; the file is flushed and
 * closed when it reaches zero. Single-process, init/shutdown on known threads,
 * so a plain (non-atomic) refcount is sufficient -- there is never a concurrent
 * open/close. Concurrent *writes* by different threads are safe because each
 * row is a single stdio fprintf (stdio locks the stream per call). */
FILE *txcsv_acquire(const char *path);
void  txcsv_release(void);

#endif /* TX_TX_CSV_H */
