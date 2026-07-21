#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "../apps/pivid-config-agent/src/modprobe_gen.h"

int main(void) {
    Settings s; settings_load_from_file(&s, "/nonexistent");
    s.flrc_cr = 1; s.flrc_pld_len = 511;
    char path[] = "/tmp/pivid_modprobe_XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);

    assert(modprobe_gen_lr2021(&s, path) == 0);
    FILE *f = fopen(path, "r"); assert(f);
    char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = '\0'; fclose(f);
    assert(strstr(buf, "options lr2021 "));
    assert(strstr(buf, "flrc_cr=1"));
    assert(strstr(buf, "flrc_mode=") == NULL);   /* knob removed: DYNAMIC only */
    assert(strstr(buf, "flrc_pld_len=511"));
    assert(strstr(buf, "flrc_crc_width="));
    assert(strstr(buf, "rx_boost=4"));   /* RX_BOOST default rides the options line */
    assert(strstr(buf, "flrc_preamble_len="));
    assert(strstr(buf, "flrc_sync_len="));
    assert(strstr(buf, "flrc_tx_low_thresh="));
    assert(strstr(buf, "rf_freq_hz=2100000000") != NULL);  /* default (2100 MHz, on the 4 MHz FE-cal grid) */
    /* Conf value emitted verbatim on every role: the RX-role -18 dBm PA pin
     * was reverted 2026-07-06 (suspected RSSI interaction, unproven). */
    assert(strstr(buf, "rf_tx_power_dbm=-3") != NULL);  /* default drives ext SZM-2166Y PA to ~2 W */
    unlink(path);
    printf("All modprobe_gen tests passed!\n");
    return 0;
}
