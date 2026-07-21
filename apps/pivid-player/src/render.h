/* apps/pivid-player/src/render.h -- SDL2 presentation. Owns the window, the
 * video YUV texture (NV12 or IYUV, recreated on size/format change), and a
 * window-resolution Cairo ARGB OSD texture blended over the video with a
 * premultiplied-alpha blend mode (cairo emits premultiplied ARGB32). When there
 * is no usable video frame, clears to black and still draws the OSD + the
 * state banner. */
#ifndef PLAYER_RENDER_H
#define PLAYER_RENDER_H
#include <stdbool.h>
#include <stdint.h>
#include "player_state.h"
#include "osd_grid.h"
struct AVFrame;

typedef struct Render Render;   /* opaque */

/* Create window + renderer. init_w/init_h are the initial window size. The
 * status-banner font is an OS-appropriate monospace face chosen internally
 * (macOS: Menlo, Windows: Consolas, Linux: fontconfig "monospace"). The MSP OSD
 * grid is drawn by the embedded-vector renderer (no atlas/font file). Returns
 * NULL on SDL failure.
 * `fullscreen` true => SDL_WINDOW_FULLSCREEN_DESKTOP (headless KMSDRM on the
 * RX Pi fills the HDMI framebuffer). False => resizable window (Mac/desktop). */
Render *render_create(int init_w, int init_h, bool fullscreen);
void    render_destroy(Render *r);

/* Pump SDL events; returns false when the user asked to quit (window close / Esc/Q). */
bool    render_pump_events(Render *r);

/* Draw one frame. `frame` may be NULL (no video -> black). `grid` is the latest
 * MSP OSD grid; `state` selects the banner. If `frame` is non-NULL and the window
 * has not been sized to video yet, the window resizes to the frame's dimensions
 * (v1: first-frame sizing).
 * `video_changed` is true when `frame` is a newly decoded picture this tick.
 * When neither the video nor the OSD (grid/state/blink/size) changed, the call
 * is a no-op: it skips the GPU clear/composite/present. Re-compositing every
 * loop iteration is needless display traffic that, on the RX Pi, perturbs the
 * bcm2835 SPI-DMA radio read and corrupts reception for ALL subscribers. */
void    render_present(Render *r, const struct AVFrame *frame,
                       const osd_grid_t *grid, PlayerConnState state,
                       int64_t now_ns, bool video_changed);

#endif
