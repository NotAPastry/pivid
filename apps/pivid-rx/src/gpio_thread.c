/* GPIO thread (RX): drives ONLY the activity LED (GPIO_TX_LED) solid while
 * CRC-valid records are arriving (within RX_ACTIVE_THRESH_NS). The config
 * button and config LED moved to pivid-config-agent (spec §11). No ioctls. */

#define _POSIX_C_SOURCE 200809L

#include "gpio_thread.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TICK_MS             50
#define RX_ACTIVE_THRESH_NS 750000000ull  /* 750 ms */

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *rx_gpio_thread_main(void *arg) {
    RX_GPIO_Thread *t = arg;
    while (__atomic_load_n(&t->run_flag, __ATOMIC_RELAXED)) {
        int rx_active = 0;
        if (t->last_valid_rx_ns) {
            uint64_t last = __atomic_load_n(t->last_valid_rx_ns, __ATOMIC_RELAXED);
            if (last != 0) {
                uint64_t now_ns = mono_ns();
                rx_active = (now_ns > last) && (now_ns - last) < RX_ACTIVE_THRESH_NS;
            }
        }
        gpio_set_tx_led(t->gpio, rx_active);

        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = TICK_MS * 1000000L };
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

int rx_gpio_thread_start(RX_GPIO_Thread *t, GPIO_Handle *gpio,
                         _Atomic uint64_t *last_valid_rx_ns) {
    memset(t, 0, sizeof(*t));
    t->gpio             = gpio;
    t->last_valid_rx_ns = last_valid_rx_ns;
    __atomic_store_n(&t->run_flag, 1, __ATOMIC_RELAXED);
    int rc = pthread_create(&t->tid, NULL, rx_gpio_thread_main, t);
    if (rc != 0) { errno = rc; return -1; }
    return 0;
}

void rx_gpio_thread_stop(RX_GPIO_Thread *t) {
    __atomic_store_n(&t->run_flag, 0, __ATOMIC_RELAXED);
    pthread_join(t->tid, NULL);
}
