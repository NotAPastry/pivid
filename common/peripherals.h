#ifndef COMMON_PERIPHERALS_H
#define COMMON_PERIPHERALS_H

#include <gpiod.h>

#define GPIO_TX_LED    5
#define GPIO_BIND_LED  6
#define GPIO_BIND_BTN  13

/* All three lines are bundled into a single libgpiod v2 line request.
 * Required for Debian trixie (libgpiod 2.x); the v1 per-line API was
 * removed when libgpiod2 was dropped from the archive. */
typedef struct {
    struct gpiod_chip *chip;
    struct gpiod_line_request *req;
} GPIO_Handle;

/* Which lines a given process claims. libgpiod v2 lines are exclusively owned
 * per request, so the always-on agent and the streaming daemon MUST request
 * disjoint sets: daemon = ACTIVITY (GPIO5); agent = BUTTON|CONFIG_LED (13,6). */
typedef enum {
    GPIO_LINE_ACTIVITY   = 1u << 0,  /* GPIO_TX_LED (5): TX activity / RX-receiving */
    GPIO_LINE_CONFIG_LED = 1u << 1,  /* GPIO_BIND_LED (6): config-mode LED          */
    GPIO_LINE_BUTTON     = 1u << 2,  /* GPIO_BIND_BTN (13): config button (in)      */
} GpioLineMask;

/* Claim only the requested lines. Returns 0 on success, -1 on failure. */
int gpio_init(GPIO_Handle *h, unsigned line_mask);
void gpio_close(GPIO_Handle *h);
int gpio_set_tx_led(GPIO_Handle *h, int on);
int gpio_set_bind_led(GPIO_Handle *h, int on);
int gpio_read_bind_button(GPIO_Handle *h);

#endif
