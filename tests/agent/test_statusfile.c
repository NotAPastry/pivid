#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../apps/pivid-config-agent/src/statusfile.h"

int main(void) {
    StatusMap m[STATUSFILE_MAX_KV];
    const char *txt = "k_crc=12\nrssi_dbm_neg=69\nk_rx_silent_ms=40\n";
    size_t n = statusfile_parse(txt, m, STATUSFILE_MAX_KV);
    assert(n == 3);
    assert(strcmp(statusfile_get(m, n, "k_crc"), "12") == 0);
    assert(strcmp(statusfile_get(m, n, "rssi_dbm_neg"), "69") == 0);
    assert(statusfile_get(m, n, "nope") == NULL);
    printf("All statusfile tests passed!\n");
    return 0;
}
