#include "osd_glyph_cache.h"
#include <string.h>

void osd_glyph_cache_init(OsdGlyphCache *c, osd_raster_fn raster, void *ud) {
    memset(c, 0, sizeof *c);
    c->raster = raster; c->ud = ud; c->cw = -1; c->ch = -1;
}

static void flush(OsdGlyphCache *c) {
    for (int i = 0; i < c->n; i++)
        if (c->entries[i].surf) cairo_surface_destroy(c->entries[i].surf);
    c->n = 0;
}

void osd_glyph_cache_free(OsdGlyphCache *c) { flush(c); memset(c, 0, sizeof *c); }

cairo_surface_t *osd_glyph_cache_get(OsdGlyphCache *c, uint32_t key, int cw, int ch) {
    if (cw != c->cw || ch != c->ch) { flush(c); c->cw = cw; c->ch = ch; }
    for (int i = 0; i < c->n; i++)
        if (c->entries[i].key == key)
            return c->entries[i].surf;
    if (c->n >= OSD_CACHE_MAX) return NULL;          /* full: caller draws nothing */
    cairo_surface_t *s = c->raster(c->ud, key, cw, ch);
    if (!s) return NULL;
    c->entries[c->n++] = (osd_cache_entry){ key, s };
    return s;
}
