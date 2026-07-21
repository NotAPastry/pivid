#include "player_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_discovering(void) {
    assert(player_state_eval(false, false, 0, 1000) == PLAYER_DISCOVERING);
    printf("test_discovering PASS\n");
}

static void test_acquiring(void) {
    /* resolved but no frame yet -> ACQUIRING regardless of clock. */
    assert(player_state_eval(true, false, 0, 5LL*1000000000) == PLAYER_ACQUIRING);
    printf("test_acquiring PASS\n");
}

static void test_live(void) {
    int64_t now = 10LL*1000000000;
    int64_t frame = now - 100000000;  /* 100 ms old */
    assert(player_state_eval(true, true, frame, now) == PLAYER_LIVE);
    printf("test_live PASS\n");
}

static void test_stalled(void) {
    int64_t now = 10LL*1000000000;
    int64_t frame = now - 1500000000; /* 1.5 s old > 1 s */
    assert(player_state_eval(true, true, frame, now) == PLAYER_STALLED);
    printf("test_stalled PASS\n");
}

static void test_boundary(void) {
    int64_t now = 10LL*1000000000;
    /* exactly at the stall window is still LIVE; just over is STALLED. */
    assert(player_state_eval(true, true, now - PLAYER_STALL_NS, now) == PLAYER_LIVE);
    assert(player_state_eval(true, true, now - PLAYER_STALL_NS - 1, now) == PLAYER_STALLED);
    printf("test_boundary PASS\n");
}

static void test_banners(void) {
    assert(player_state_banner(PLAYER_DISCOVERING) != NULL);
    assert(player_state_banner(PLAYER_ACQUIRING)   != NULL);
    assert(player_state_banner(PLAYER_LIVE)    == NULL);
    assert(player_state_banner(PLAYER_STALLED) == NULL);
    /* ACQUIRING banner is neutral (no "LOST"/alarming wording). */
    assert(strstr(player_state_banner(PLAYER_ACQUIRING), "keyframe") != NULL);
    printf("test_banners PASS\n");
}

int main(void) {
    test_discovering();
    test_acquiring();
    test_live();
    test_stalled();
    test_boundary();
    test_banners();
    printf("ALL test_player_state PASS\n");
    return 0;
}
