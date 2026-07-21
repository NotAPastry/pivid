/* apps/pivid-player/src/player_args.c -- see player_args.h. */
#include "player_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *player_args_usage(void) {
    return
    "Usage: pivid-player [--caster <ip>] [options]\n"
    "  --caster <ip>          caster address (default: auto-discover via mDNS)\n"
    "  --port <p>             caster control port (default: from mDNS, else 5700)\n"
    "  --video-port <p>       pin the video receive port (debug/A-B replay)\n"
    "                         default: auto-assigned ephemeral\n"
    "  --width <px>           force window width  (default: size to first frame)\n"
    "  --height <px>          force window height (default: size to first frame)\n"
    "  --hw                   hardware decode (clean streams; opt-in)\n"
    "                         default: software decode (conceals on the lossy link)\n"
    "  --fullscreen           fill the screen (headless KMSDRM / RX-Pi HDMI)\n"
    "                         default: windowed\n"
    "  --version              print version and exit\n"
    "  -h, --help             show this help\n"
    "\n"
    "The player's video + telemetry receive ports are auto-assigned and\n"
    "advertised to the caster over the control channel -- nothing to configure.\n";
}

/* Parse a uint16 port in [1,65535]; returns 0 on success, -1 on error. */
static int parse_port(const char *s, uint16_t *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 1 || v > 65535) return -1;
    *out = (uint16_t)v;
    return 0;
}

static int parse_dim(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 16 || v > 16384) return -1;
    *out = (int)v;
    return 0;
}

int player_args_parse(int argc, char **argv, PlayerArgs *out) {
    memset(out, 0, sizeof *out);
    /* port stays 0 => main resolves to mDNS/CASTER_CTRL_PORT.
     * win_w/win_h stay 0 => size to first decoded frame.
     * caster_ip stays "" => discover via mDNS. hw_decode stays false (software).
     * fullscreen stays false (windowed). */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEED_VAL() do { if (++i >= argc) return -1; } while (0)
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { out->show_help = true; return 1; }
        else if (strcmp(a, "--version") == 0) { out->show_version = true; }
        else if (strcmp(a, "--hw") == 0) { out->hw_decode = true; }
        else if (strcmp(a, "--fullscreen") == 0) { out->fullscreen = true; }
        else if (strcmp(a, "--caster") == 0) {
            NEED_VAL(); snprintf(out->caster_ip, sizeof out->caster_ip, "%s", argv[i]);
        } else if (strcmp(a, "--port") == 0) {
            NEED_VAL(); if (parse_port(argv[i], &out->port) != 0) return -1;
        } else if (strcmp(a, "--video-port") == 0) {
            NEED_VAL(); if (parse_port(argv[i], &out->video_port) != 0) return -1;
        } else if (strcmp(a, "--width") == 0) {
            NEED_VAL(); if (parse_dim(argv[i], &out->win_w) != 0) return -1;
        } else if (strcmp(a, "--height") == 0) {
            NEED_VAL(); if (parse_dim(argv[i], &out->win_h) != 0) return -1;
        } else {
            return -1;   /* unknown flag */
        }
        #undef NEED_VAL
    }
    return 0;   /* missing --caster is OK: main auto-discovers via mDNS */
}
