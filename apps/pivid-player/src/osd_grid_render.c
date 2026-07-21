/* apps/pivid-player/src/osd_grid_render.c -- blit-based OSD grid renderer using
 * the embedded msp-osd bitmap font (page 0, 256 glyphs). Each cell's glyph is
 * downscaled from the 36x54 font atlas and cached by glyph index + cell size. */
#include "osd_grid_render.h"
#include "osd_glyph_cache.h"
#include <stdlib.h>
#include <stdbool.h>

/* BLINK_PERIOD_NS / BLINK_ON now live in osd_grid_render.h (shared with render.c
 * so the present loop can skip re-rasterizing the OSD on unchanged frames). */

struct OsdGridRender {
    const OsdMspFont *font;
    OsdGlyphCache     cache;
};

/* Rasterise glyph `key` (page-0 BF index 0..255) into a new cw×ch surface. */
static cairo_surface_t *raster(void *ud, uint32_t key, int cw, int ch) {
    OsdGridRender *r = ud;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
    cairo_t *cr = cairo_create(s);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    double sx, sy;
    osd_msp_font_src((unsigned)key, &sx, &sy);
    cairo_scale(cr, (double)cw / OSD_FONT_GW, (double)ch / OSD_FONT_GH);
    cairo_set_source_surface(cr, osd_msp_font_sheet(r->font), -sx, -sy);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_rectangle(cr, 0, 0, OSD_FONT_GW, OSD_FONT_GH);
    cairo_clip(cr);
    cairo_paint(cr);

    cairo_destroy(cr);
    return s;
}

OsdGridRender *osd_grid_render_create(const OsdMspFont *font) {
    OsdGridRender *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->font = font;
    osd_glyph_cache_init(&r->cache, raster, r);
    return r;
}

void osd_grid_render_destroy(OsdGridRender *r) {
    if (!r) return;
    osd_glyph_cache_free(&r->cache);
    free(r);
}

void osd_grid_render_draw(OsdGridRender *r, cairo_t *cr, const osd_grid_t *g,
                          int frame_w, int frame_h, int64_t now_ns) {
    if (frame_w <= 0 || frame_h <= 0) return;

    cairo_save(cr);

    double cw = (double)frame_w / OSD_GRID_COLS;
    double ch = (double)frame_h / OSD_GRID_ROWS;
    int icw = (int)(cw + 0.5), ich = (int)(ch + 0.5);
    bool blink_on = BLINK_ON(now_ns);

    for (unsigned row = 0; row < OSD_GRID_ROWS; row++) {
        for (unsigned col = 0; col < OSD_GRID_COLS; col++) {
            const osd_cell_t *cell = &g->cells[row * OSD_GRID_COLS + col];
            if (cell->ch == OSD_GRID_BLANK) continue;
            if ((cell->attr & OSD_ATTR_BLINK) && !blink_on) continue;
            cairo_surface_t *s = osd_glyph_cache_get(&r->cache, cell->ch, icw, ich);
            if (!s) continue;
            cairo_save(cr);
            cairo_rectangle(cr, col * cw, row * ch, icw, ich);
            cairo_clip(cr);
            cairo_set_source_surface(cr, s,
                (double)(int)(col * cw + 0.5),
                (double)(int)(row * ch + 0.5));
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }

    cairo_restore(cr);
}
