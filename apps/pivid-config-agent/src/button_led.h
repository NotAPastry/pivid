#ifndef AGENT_BUTTON_LED_H
#define AGENT_BUTTON_LED_H
#include <pthread.h>
#include <stdatomic.h>
#include "peripherals.h"

typedef struct {
    GPIO_Handle *gpio;       /* claims {BUTTON, CONFIG_LED}; owned for lifetime */
    _Atomic int  run_flag;
    pthread_t    tid;
} ButtonLed;

int  button_led_start(ButtonLed *b, GPIO_Handle *gpio);
void button_led_stop(ButtonLed *b);
#endif
