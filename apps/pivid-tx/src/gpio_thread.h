#ifndef TX_GPIO_THREAD_H
#define TX_GPIO_THREAD_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "encoder.h"
#include "peripherals.h"

typedef struct {
    GPIO_Handle      *gpio;      /* owned by thread for its lifetime */
    Encoder          *encoder;   /* thread reads encoder->last_data_ns */
    _Atomic int       run_flag;  /* 0 -> thread exits */
    pthread_t         tid;
} GPIO_Thread;

int  gpio_thread_start(GPIO_Thread *t, GPIO_Handle *gpio, Encoder *encoder);

/* Set run_flag=0 and pthread_join. After return, the GPIO_Handle is the
 * caller's again; caller should set LEDs off and gpio_close. */
void gpio_thread_stop(GPIO_Thread *t);

#endif
