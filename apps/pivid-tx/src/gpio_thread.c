/* GPIO thread (TX): drives ONLY the activity LED (GPIO_TX_LED) at 0.5 Hz while
 * the encoder has touched the ring within the last 1 s. The config button and
 * config LED moved to pivid-config-agent (spec §11). No ioctls. */

#define _POSIX_C_SOURCE 200809L

#include "gpio_thread.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TICK_MS      50
#define LED_BLINK_MS 500      /* 0.5 Hz -> 500 ms on, 500 ms off */
#define TX_IDLE_MS  1000

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void *gpio_thread_main(void *arg) {
    GPIO_Thread *t = arg;
    while (__atomic_load_n(&t->run_flag, __ATOMIC_RELAXED)) {
        uint64_t now_ms = mono_ms();
        uint64_t last_data_ns = __atomic_load_n(&t->encoder->last_data_ns,
                                                __ATOMIC_RELAXED);
        int tx_led_on;
        if (last_data_ns == 0) {
            tx_led_on = 0;
        } else {
            uint64_t last_data_ms = last_data_ns / 1000000ull;
            uint64_t idle_ms = (now_ms > last_data_ms) ? (now_ms - last_data_ms) : 0;
            tx_led_on = (idle_ms > TX_IDLE_MS)
                          ? 0 : (int)((now_ms / LED_BLINK_MS) & 1ull);
        }
        gpio_set_tx_led(t->gpio, tx_led_on);

        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = TICK_MS * 1000000L };
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

int gpio_thread_start(GPIO_Thread *t, GPIO_Handle *gpio, Encoder *encoder) {
    memset(t, 0, sizeof(*t));
    t->gpio    = gpio;
    t->encoder = encoder;
    __atomic_store_n(&t->run_flag, 1, __ATOMIC_RELAXED);
    int rc = pthread_create(&t->tid, NULL, gpio_thread_main, t);
    if (rc != 0) { errno = rc; return -1; }
    return 0;
}

void gpio_thread_stop(GPIO_Thread *t) {
    __atomic_store_n(&t->run_flag, 0, __ATOMIC_RELAXED);
    pthread_join(t->tid, NULL);
}
