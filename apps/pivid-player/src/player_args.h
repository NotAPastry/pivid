/* apps/pivid-player/src/player_args.h -- CLI parsing for pivid-player.
 *
 * The player is configured ENTIRELY by command-line switches -- there is no
 * config file. The parser is PURE (no sockets): it fills a PlayerArgs already
 * seeded with built-in defaults, and each switch overrides its field.
 *
 * Run with no switches -> caster_ip stays "" -> main.c auto-discovers a caster
 * via mDNS. The caster's control port is the only externally-meaningful port
 * (--port; else taken from mDNS, else CASTER_CTRL_PORT). The player's own video
 * + telemetry RECEIVE ports are auto-assigned (ephemeral) and advertised to the
 * caster over the control channel -- there is nothing to configure. Decode
 * defaults to software (conceals on the lossy link); --hw opts into hardware. */
#ifndef PLAYER_ARGS_H
#define PLAYER_ARGS_H
#include <stdbool.h>
#include <stdint.h>
#include "attribution.h"   /* PIVID_ATTRIBUTION -- GPL-3.0 s7(b), see LICENSE */

#define PLAYER_DEFAULT_WIN_W      960
#define PLAYER_DEFAULT_WIN_H      540

typedef struct {
    char     caster_ip[64];   /* "" => discover via mDNS */
    uint16_t port;            /* caster control port; 0 => mDNS/CASTER_CTRL_PORT */
    uint16_t video_port;      /* --video-port: pin the video RECV port (debug/A-B);
                                 0 => ephemeral (production default). */
    int      win_w, win_h;    /* 0 => size to first decoded frame */
    bool     hw_decode;       /* default false (software); --hw sets true */
    bool     fullscreen;      /* --fullscreen: SDL_WINDOW_FULLSCREEN_DESKTOP
                                 (headless KMSDRM on the RX Pi); default windowed */
    bool     show_help;
    bool     show_version;
} PlayerArgs;

/* Fill *out with defaults, then overlay any switches from argv. Returns 0 on
 * success, -1 on a bad/unknown flag or missing value, 1 if --help/-h was given
 * (caller prints usage + exits 0). --version sets show_version (caller prints +
 * exits 0). A missing --caster is NOT an error (main auto-discovers via mDNS). */
int player_args_parse(int argc, char **argv, PlayerArgs *out);

/* The usage string (for --help and parse errors). */
const char *player_args_usage(void);

#endif
