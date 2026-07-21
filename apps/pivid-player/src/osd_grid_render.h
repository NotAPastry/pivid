/* apps/pivid-player/src/osd_grid_render.h -- draw the 30x16 OSD grid by blitting
 * each cell's glyph from the embedded msp-osd font (page 0), downscaled + cached.
 * Blink: attr bit 0x40, 2 Hz local oscillator. */
#ifndef PLAYER_OSD_GRID_RENDER_H
#define PLAYER_OSD_GRID_RENDER_H
#include <cairo.h>
#include <stdint.h>
#include "osd_grid.h"
#include "osd_msp_font.h"

#define OSD_ATTR_BLINK 0x40u

/* 2 Hz blink oscillator, shared with the renderer's caller so it can decide when
 * the composited OSD actually changes (and avoid re-rasterizing every frame). */
#define BLINK_PERIOD_NS 500000000LL
#define BLINK_ON(t)     (((t) / BLINK_PERIOD_NS) % 2 == 0)

typedef struct OsdGridRender OsdGridRender;
OsdGridRender *osd_grid_render_create(const OsdMspFont *font);
void           osd_grid_render_destroy(OsdGridRender *r);
void osd_grid_render_draw(OsdGridRender *r, cairo_t *cr, const osd_grid_t *g,
                          int frame_w, int frame_h, int64_t now_ns);
#endif
