#include "player_args.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char *AV(const char *s) { return (char *)s; }

/* Parser seeds defaults; unset switches leave the default value. */
static void test_defaults_seeded(void) {
    char *argv[] = { AV("pivid-player") };
    PlayerArgs a;
    assert(player_args_parse(1, argv, &a) == 0);
    assert(a.caster_ip[0] == '\0');               /* no caster => mDNS */
    assert(a.port == 0);                          /* 0 => main resolves */
    assert(a.win_w == 0 && a.win_h == 0);         /* size to first frame */
    assert(a.hw_decode == false);                 /* software default */
    assert(a.fullscreen == false);                /* windowed default */
    printf("test_defaults_seeded PASS\n");
}

static void test_caster_port_and_dims(void) {
    char *argv[] = {
        AV("pivid-player"), AV("--caster"), AV("10.0.0.5"),
        AV("--port"), AV("5800"), AV("--width"), AV("1280") };
    PlayerArgs a;
    assert(player_args_parse(7, argv, &a) == 0);
    assert(strcmp(a.caster_ip, "10.0.0.5") == 0);
    assert(a.port == 5800);
    assert(a.win_w == 1280);
    printf("test_caster_port_and_dims PASS\n");
}

static void test_hw_flag(void) {
    char *argv[] = { AV("pivid-player"), AV("--hw") };
    PlayerArgs a;
    assert(player_args_parse(2, argv, &a) == 0);
    assert(a.hw_decode == true);
    printf("test_hw_flag PASS\n");
}

static void test_version_and_help(void) {
    PlayerArgs a;
    char *v[] = { AV("pivid-player"), AV("--version") };
    assert(player_args_parse(2, v, &a) == 0 && a.show_version);
    char *h[] = { AV("pivid-player"), AV("--help") };
    assert(player_args_parse(2, h, &a) == 1);
    printf("test_version_and_help PASS\n");
}

static void test_video_port(void) {
    PlayerArgs a;
    /* --video-port pins the video receive port (debug/A-B). */
    char *argv[] = { AV("pivid-player"), AV("--video-port"), AV("6001") };
    assert(player_args_parse(3, argv, &a) == 0);
    assert(a.video_port == 6001);
    /* a non-numeric / out-of-range video port (0) is rejected. */
    char *bad[] = { AV("pivid-player"), AV("--video-port"), AV("0") };
    assert(player_args_parse(3, bad, &a) == -1);
    printf("test_video_port PASS\n");
}

static void test_fullscreen_flag(void) {
    char *argv[] = { AV("pivid-player"), AV("--fullscreen") };
    PlayerArgs a;
    assert(player_args_parse(2, argv, &a) == 0);
    assert(a.fullscreen == true);
    printf("test_fullscreen_flag PASS\n");
}

static void test_bad_flag_and_value(void) {
    PlayerArgs a;
    char *b1[] = { AV("pivid-player"), AV("--nope") };
    assert(player_args_parse(2, b1, &a) == -1);
    char *b2[] = { AV("pivid-player"), AV("--port"), AV("99999") };
    assert(player_args_parse(3, b2, &a) == -1);
    char *b3[] = { AV("pivid-player"), AV("--caster") };   /* missing value */
    assert(player_args_parse(2, b3, &a) == -1);
    /* removed switches are now unknown flags */
    char *b4[] = { AV("pivid-player"), AV("--sw") };
    assert(player_args_parse(2, b4, &a) == -1);
    char *b5[] = { AV("pivid-player"), AV("--config"), AV("/tmp/p.conf") };
    assert(player_args_parse(3, b5, &a) == -1);
    /* --video-port "0" is out-of-range and must be rejected. */
    char *b6[] = { AV("pivid-player"), AV("--video-port"), AV("0") };
    assert(player_args_parse(3, b6, &a) == -1);
    char *b7[] = { AV("pivid-player"), AV("--telem-port"), AV("5001") };
    assert(player_args_parse(3, b7, &a) == -1);
    char *b8[] = { AV("pivid-player"), AV("--control-port"), AV("5700") };
    assert(player_args_parse(3, b8, &a) == -1);
    printf("test_bad_flag_and_value PASS\n");
}

int main(void) {
    test_defaults_seeded();
    test_caster_port_and_dims();
    test_hw_flag();
    test_fullscreen_flag();
    test_version_and_help();
    test_video_port();
    test_bad_flag_and_value();
    printf("ALL test_player_args PASS\n");
    return 0;
}
