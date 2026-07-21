#ifndef RX_GPIO_THREAD_H
#define RX_GPIO_THREAD_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "peripherals.h"

typedef struct {
    GPIO_Handle      *gpio;
    _Atomic uint64_t *last_valid_rx_ns;  /* RX_Loop.last_valid_rx_ns */
    _Atomic int       run_flag;
    pthread_t         tid;
} RX_GPIO_Thread;

int  rx_gpio_thread_start(RX_GPIO_Thread *t, GPIO_Handle *gpio,
                          _Atomic uint64_t *last_valid_rx_ns);

/* Set run_flag=0 and pthread_join. After return, the GPIO_Handle is the
 * caller's again; caller should set LEDs off and gpio_close. */
void rx_gpio_thread_stop(RX_GPIO_Thread *t);

#endif
