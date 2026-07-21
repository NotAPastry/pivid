/* apps/pivid-player/src/player_state.h -- the player's connection state machine.
 *
 * Pure: a function of liveness inputs and the player's own monotonic clock. The
 * render loop calls player_state_eval() each frame to pick the status banner and
 * whether to draw video or OSD-over-black. See design §4 "Connection lifecycle".
 *   DISCOVERING -- caster not yet resolved (transient in v1 --caster mode)
 *   ACQUIRING   -- subscribed, no decoded frame yet (pre-IDR)
 *   LIVE        -- a decoded frame newer than the stall window
 *   STALLED     -- had a frame, none within the stall window
 */
#ifndef PLAYER_STATE_H
#define PLAYER_STATE_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PLAYER_DISCOVERING = 0,
    PLAYER_ACQUIRING,
    PLAYER_LIVE,
    PLAYER_STALLED,
} PlayerConnState;

#define PLAYER_STALL_NS  1000000000LL   /* 1 s: video stall threshold */

/* resolved:      caster target known (true once args parsed, in v1 --caster mode)
 * ever_decoded:  at least one frame has been decoded since start
 * last_frame_ns: monotonic timestamp of the newest decoded frame (only meaningful
 *                when ever_decoded); now_ns: the render loop's current clock. */
PlayerConnState player_state_eval(bool resolved, bool ever_decoded,
                                  int64_t last_frame_ns, int64_t now_ns);

/* The centered status banner for a state, or NULL when none should be drawn
 * (LIVE). STALLED returns NULL too -- the render loop drives its own video-dead
 * banner via the video_dead flag on the OSD grid, independently of this path. */
const char *player_state_banner(PlayerConnState st);

#endif
