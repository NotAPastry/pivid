/* pivid-config-agent: always-on config plane. Starts civetweb on 0.0.0.0:80,
 * runs the bind-button/LED thread, serves the schema-driven settings UI and the
 * /api routes, and blocks until SIGTERM. Never opens /dev/radio0 -- it drives
 * the radio only indirectly, by writing pivid.conf + /etc/modprobe.d and then
 * signalling or restarting the pivid-tx / pivid-rx daemon (see apply tiers in
 * common/config_schema.c). */
#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "role.h"
#include "http.h"
#include "civetweb.h"
#include "peripherals.h"
#include "button_led.h"
#include "settings.h"
#include "cli_args.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_term(int sig) { (void)sig; g_stop = 1; }

/* Build stamp (BUILD_VERSION from the Makefile). This TU is force-rebuilt so the
 * stamp always reflects the current tree; http.c reads it via extern. */
const char *g_pivid_version = BUILD_VERSION;

/* Runtime config path (see agent.h). Written once in main() before mg_start. */
const char *g_agent_conf_path = AGENT_PIVID_CONF;

int main(int argc, char **argv) {
    CliArgs args;
    if (cli_parse(argc, argv, AGENT_PIVID_CONF, &args) != 0) {
        cli_print_usage(stderr, "pivid-config-agent");
        return 2;
    }
    if (args.show_help)    { cli_print_usage(stdout, "pivid-config-agent"); return 0; }
    if (args.show_version) { printf("pivid-config-agent %s\n%s\n", BUILD_VERSION, PIVID_ATTRIBUTION); return 0; }
    if (args.check) {
        char err[128];
        if (settings_check_file(args.config_path, err, sizeof err) != 0) {
            fprintf(stderr, "pivid-config-agent: config invalid: %s\n", err);
            return 1;
        }
        printf("pivid-config-agent: config OK\n");
        return 0;
    }
    /* Point every runtime load/save at --config (default AGENT_PIVID_CONF).
     * Set here, before mg_start, so no civetweb worker can observe the old
     * value. args.config_path points into argv, which outlives this process. */
    g_agent_conf_path = args.config_path;

    /* No startup fail-fast: the agent is the RECOVERY tool and must come up
     * even on a corrupt config so the user can fix it via the web UI. */

    AgentRole role = role_detect();
    fprintf(stderr, "pivid-config-agent: role=%s build=%s\n",
            role_daemon_unit(role), g_pivid_version);

    signal(SIGTERM, on_term);
    signal(SIGINT,  on_term);
    signal(SIGPIPE, SIG_IGN);

    GPIO_Handle gpio;
    int have_gpio = (gpio_init(&gpio, GPIO_LINE_BUTTON | GPIO_LINE_CONFIG_LED) == 0);
    ButtonLed btn;
    if (have_gpio) {
        gpio_set_bind_led(&gpio, 0);
        if (button_led_start(&btn, &gpio) < 0) {
            fprintf(stderr, "pivid-config-agent: button thread failed; continuing web-only\n");
            gpio_close(&gpio);
            have_gpio = 0;
        }
    } else {
        fprintf(stderr, "pivid-config-agent: gpio_init failed; continuing web-only\n");
    }

    const char *opts[] = {
        "listening_ports", "0.0.0.0:" AGENT_HTTP_PORT,
        "num_threads",     "4",
        "enable_directory_listing", "no",
        NULL
    };
    struct mg_callbacks cb; memset(&cb, 0, sizeof(cb));
    struct mg_context *ctx = mg_start(&cb, NULL, opts);
    if (!ctx) {
        fprintf(stderr, "pivid-config-agent: mg_start failed\n");
        if (have_gpio) { button_led_stop(&btn); gpio_set_bind_led(&gpio, 0); gpio_close(&gpio); }
        return 1;
    }
    http_register(ctx, role);

    while (!g_stop) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
        nanosleep(&ts, NULL);
    }
    mg_stop(ctx);
    if (have_gpio) { button_led_stop(&btn); gpio_set_bind_led(&gpio, 0); gpio_close(&gpio); }
    fprintf(stderr, "pivid-config-agent: exit\n");
    return 0;
}
