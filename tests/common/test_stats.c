/* Host unit test for common/status_file.c (the pure key=value formatter +
 * atomic writer) and the radio_stats ABI layout. */
#include "status_file.h"
#include "radio_ioctl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static void test_format_basic(void) {
    StatusKV kv[] = {
        { "packets_tx", 10 },
        { "bytes_tx", 2550 },
        { "errors_irq", 0 },
    };
    char buf[256];
    int n = status_format(buf, sizeof(buf), kv, 3);
    assert(n > 0);
    assert(strcmp(buf, "packets_tx=10\nbytes_tx=2550\nerrors_irq=0\n") == 0);
    /* return value is the byte length written (excluding NUL) */
    assert((size_t)n == strlen(buf));
    printf("  test_format_basic OK (%d bytes)\n", n);
}

static void test_format_truncation_fails(void) {
    StatusKV kv[] = { { "a", 1 }, { "b", 2 } };
    char tiny[5];               /* too small for even one line */
    int n = status_format(tiny, sizeof(tiny), kv, 2);
    assert(n == -1);            /* must refuse to overflow */
    printf("  test_format_truncation_fails OK\n");
}

static void test_write_atomic(void) {
    StatusKV kv[] = { { "nals", 7 }, { "fec_rec", 3 } };
    const char *path = "/tmp/pivid_test_stats.status";
    unlink(path);
    int rc = status_write(path, kv, 2);
    assert(rc == 0);
    FILE *f = fopen(path, "r");
    assert(f);
    char got[128]; size_t r = fread(got, 1, sizeof(got) - 1, f); got[r] = 0;
    fclose(f);
    assert(strcmp(got, "nals=7\nfec_rec=3\n") == 0);
    unlink(path);
    printf("  test_write_atomic OK\n");
}

static void test_abi_layout(void) {
    /* Lock the wire ABI: if the struct changes, RADIO_STATS_VERSION must
     * bump and this expected size must be updated deliberately. */
    assert(RADIO_STATS_VERSION == 1u);
    /* 2*u32 + 7*u64 + 11*u32 + 1*u32 pad = 8 + 56 + 48 = 112 */
    assert(sizeof(struct radio_stats) == 112);
    printf("  test_abi_layout OK (sizeof=%zu)\n", sizeof(struct radio_stats));
}

int main(void) {
    printf("test_stats:\n");
    test_format_basic();
    test_format_truncation_fails();
    test_write_atomic();
    test_abi_layout();
    printf("test_stats: all passed\n");
    return 0;
}
