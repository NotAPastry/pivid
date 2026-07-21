#include "peripherals.h"
#include <stdio.h>

int gpio_init(GPIO_Handle *h, unsigned line_mask) {
    if (!h) return -1;
    h->chip = NULL;
    h->req  = NULL;

    struct gpiod_line_settings  *out_settings = NULL;
    struct gpiod_line_settings  *in_settings  = NULL;
    struct gpiod_line_config    *line_cfg     = NULL;
    struct gpiod_request_config *req_cfg      = NULL;

    h->chip = gpiod_chip_open("/dev/gpiochip0");
    if (!h->chip) { perror("gpiod_chip_open"); return -1; }

    out_settings = gpiod_line_settings_new();
    in_settings  = gpiod_line_settings_new();
    line_cfg     = gpiod_line_config_new();
    req_cfg      = gpiod_request_config_new();
    if (!out_settings || !in_settings || !line_cfg || !req_cfg) {
        fprintf(stderr, "libgpiod allocation failed\n");
        goto fail;
    }

    if (gpiod_line_settings_set_direction(out_settings,
            GPIOD_LINE_DIRECTION_OUTPUT) < 0 ||
        gpiod_line_settings_set_output_value(out_settings,
            GPIOD_LINE_VALUE_INACTIVE) < 0 ||
        gpiod_line_settings_set_direction(in_settings,
            GPIOD_LINE_DIRECTION_INPUT) < 0 ||
        gpiod_line_settings_set_bias(in_settings,
            GPIOD_LINE_BIAS_PULL_UP) < 0) {
        perror("gpiod_line_settings_set_*");
        goto fail;
    }
    /* The button wires to GND on press and floats when released, so it needs
     * the pull-up above plus active-low: a press pulls the line LOW, mapped to
     * logical ACTIVE -> gpio_read_bind_button() returns 1. (HW-measured on
     * GPIO13.) */
    gpiod_line_settings_set_active_low(in_settings, true);

    /* Add only the requested LED outputs. */
    unsigned int led_offsets[2];
    size_t n_leds = 0;
    if (line_mask & GPIO_LINE_ACTIVITY)   led_offsets[n_leds++] = GPIO_TX_LED;
    if (line_mask & GPIO_LINE_CONFIG_LED) led_offsets[n_leds++] = GPIO_BIND_LED;
    if (n_leds &&
        gpiod_line_config_add_line_settings(line_cfg, led_offsets, n_leds,
                                            out_settings) < 0) {
        perror("gpiod_line_config_add_line_settings(out)");
        goto fail;
    }

    if (line_mask & GPIO_LINE_BUTTON) {
        const unsigned int btn_offsets[1] = { GPIO_BIND_BTN };
        if (gpiod_line_config_add_line_settings(line_cfg, btn_offsets, 1,
                                                in_settings) < 0) {
            perror("gpiod_line_config_add_line_settings(in)");
            goto fail;
        }
    }

    gpiod_request_config_set_consumer(req_cfg, "pivid");

    h->req = gpiod_chip_request_lines(h->chip, req_cfg, line_cfg);
    if (!h->req) { perror("gpiod_chip_request_lines"); goto fail; }

    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(in_settings);
    gpiod_line_settings_free(out_settings);
    return 0;

fail:
    if (req_cfg)      gpiod_request_config_free(req_cfg);
    if (line_cfg)     gpiod_line_config_free(line_cfg);
    if (in_settings)  gpiod_line_settings_free(in_settings);
    if (out_settings) gpiod_line_settings_free(out_settings);
    gpiod_chip_close(h->chip);
    h->chip = NULL;
    return -1;
}

void gpio_close(GPIO_Handle *h) {
    if (!h) return;
    if (h->req)  { gpiod_line_request_release(h->req); h->req  = NULL; }
    if (h->chip) { gpiod_chip_close(h->chip);          h->chip = NULL; }
}

int gpio_set_tx_led(GPIO_Handle *h, int on) {
    if (!h || !h->req) return -1;
    return gpiod_line_request_set_value(h->req, GPIO_TX_LED,
        on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

int gpio_set_bind_led(GPIO_Handle *h, int on) {
    if (!h || !h->req) return -1;
    return gpiod_line_request_set_value(h->req, GPIO_BIND_LED,
        on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

int gpio_read_bind_button(GPIO_Handle *h) {
    if (!h || !h->req) return 0;
    enum gpiod_line_value v =
        gpiod_line_request_get_value(h->req, GPIO_BIND_BTN);
    return (v == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
}
