/* apps/pivid-player/src/player_state.c -- see player_state.h. */
#include "player_state.h"

PlayerConnState player_state_eval(bool resolved, bool ever_decoded,
                                  int64_t last_frame_ns, int64_t now_ns) {
    if (!resolved)      return PLAYER_DISCOVERING;
    if (!ever_decoded)  return PLAYER_ACQUIRING;
    if (now_ns - last_frame_ns > PLAYER_STALL_NS) return PLAYER_STALLED;
    return PLAYER_LIVE;
}

const char *player_state_banner(PlayerConnState st) {
    switch (st) {
        case PLAYER_DISCOVERING: return "Searching for caster…";
        case PLAYER_ACQUIRING:   return "Waiting for video — next keyframe…";
        default:                 return 0;   /* LIVE + STALLED draw no banner here */
    }
}
