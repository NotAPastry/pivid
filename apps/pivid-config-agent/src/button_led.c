/* Agent button + config-LED thread (spec §4.0/§4.2/§11.2). 50 ms poll, 100 ms
 * debounce, 4.8 s hold -> toggle pivid-config-net.service. Config LED: countdown
 * while held (six 0.8 s half-phases), post-fire override to the target state,
 * else follow /run/pivid/config-mode (rapid ~5 Hz blink while up, dark
 * otherwise). The countdown's first half-phase is the visual opposite of the
 * resting LED so the press registers instantly, and it lands on the new mode's
 * resting state: client mode (rests dark) -> on/off/on/off/on/off -> rapid
 * blink; AP mode (rests blinking) -> off/on/off/on/off/on -> permanent off. */
#define _POSIX_C_SOURCE 200809L

#include "button_led.h"
#include "agent.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TICK_MS               50
#define DEBOUNCE_TICKS         2
#define LONG_PRESS_MS       4800
#define CONFIG_LED_BLINK_MS  100
#define COUNTDOWN_HALF_MS    800
#define OVERRIDE_MAX_MS    12000
#define FLAG_POLL_MS         500
#define SYSTEMCTL "/usr/bin/systemctl"

static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Toggle the config-mode net unit. Double-fork: grandchild execs systemctl and
 * is reparented to init. Absolute path (no PATH malloc after fork). Returns 1 if
 * it STARTED config mode (going to AP), 0 if it stopped it -- the caller uses
 * this fire-time decision to drive the post-fire LED, so the LED can never
 * disagree with the action actually taken. */
static int net_unit_toggle(void) {
    int going_to_ap = (access(AGENT_CONFIG_MODE_FLAG, F_OK) != 0);
    const char *action = going_to_ap ? "start" : "stop";
    fprintf(stderr, "pivid-config-agent: config-mode %s\n", action);
    pid_t pid = fork();
    if (pid == 0) {
        pid_t gc = fork();
        if (gc == 0) {
            execl(SYSTEMCTL, "systemctl", "--no-block", action,
                  AGENT_NET_UNIT, (char *)NULL);
            _exit(127);
        }
        _exit(gc < 0 ? 127 : 0);
    }
    if (pid > 0) (void)waitpid(pid, NULL, 0);
    return going_to_ap;
}

static void *button_led_main(void *arg) {
    ButtonLed *t = arg;
    int pressed_streak = 0, released_streak = 0, is_held = 0;
    uint64_t held_since_ms = 0, last_flag_check_ms = 0, override_since_ms = 0;
    int long_press_already_fired = 0, config_mode = 0;
    int press_intent_starting = 0, post_fire_override = 0, override_starting = 0;

    while (__atomic_load_n(&t->run_flag, __ATOMIC_RELAXED)) {
        uint64_t now_ms = mono_ms();

        int raw = gpio_read_bind_button(t->gpio);
        if (raw > 0) { pressed_streak++;  released_streak = 0; }
        else         { released_streak++; pressed_streak  = 0; }

        if (!is_held && pressed_streak >= DEBOUNCE_TICKS) {
            is_held = 1; held_since_ms = now_ms; long_press_already_fired = 0;
            post_fire_override = 0;
            config_mode = (access(AGENT_CONFIG_MODE_FLAG, F_OK) == 0);
            press_intent_starting = !config_mode;
        } else if (is_held && released_streak >= DEBOUNCE_TICKS) {
            is_held = 0; long_press_already_fired = 0;
        }

        if (is_held && !long_press_already_fired &&
            (now_ms - held_since_ms) >= LONG_PRESS_MS) {
            long_press_already_fired = 1;
            /* Drive the post-fire LED from the SAME fire-time decision as the
             * action (not the press-time latch): a press made during AP bring-up
             * sees no flag yet, so press_intent_starting can disagree with the
             * action actually taken 4.8s later -> would briefly show the wrong
             * state (e.g. rapid-blink while really shutting AP down). */
            int going_to_ap = net_unit_toggle();
            post_fire_override = 1; override_starting = going_to_ap;
            override_since_ms = now_ms;
        }

        if (now_ms - last_flag_check_ms >= FLAG_POLL_MS) {
            config_mode = (access(AGENT_CONFIG_MODE_FLAG, F_OK) == 0);
            last_flag_check_ms = now_ms;
        }
        int led_on;
        if (is_held && !long_press_already_fired) {
            uint64_t held_ms = now_ms - held_since_ms;
            int phase = (int)((held_ms / COUNTDOWN_HALF_MS) & 1ull);
            /* press_intent_starting == client mode (going TO ap): rest is dark,
             * so start ON -> on/off/on/off/on/off. Else (in ap, going to
             * client): rest is rapid-blink, so start steady OFF ->
             * off/on/off/on/off/on. Both run to the long-press fire below. */
            led_on = press_intent_starting ? (phase == 0) : (phase == 1);
        } else if (post_fire_override) {
            led_on = override_starting ? (int)((now_ms / CONFIG_LED_BLINK_MS) & 1ull) : 0;
            int flag_agrees = override_starting ? config_mode : !config_mode;
            if (flag_agrees || (now_ms - override_since_ms) >= OVERRIDE_MAX_MS)
                post_fire_override = 0;
        } else {
            led_on = config_mode ? (int)((now_ms / CONFIG_LED_BLINK_MS) & 1ull) : 0;
        }
        gpio_set_bind_led(t->gpio, led_on);

        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = TICK_MS * 1000000L };
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

int button_led_start(ButtonLed *b, GPIO_Handle *gpio) {
    memset(b, 0, sizeof(*b));
    b->gpio = gpio;
    __atomic_store_n(&b->run_flag, 1, __ATOMIC_RELAXED);
    int rc = pthread_create(&b->tid, NULL, button_led_main, b);
    if (rc != 0) { errno = rc; return -1; }
    return 0;
}

void button_led_stop(ButtonLed *b) {
    __atomic_store_n(&b->run_flag, 0, __ATOMIC_RELAXED);
    pthread_join(b->tid, NULL);
}
