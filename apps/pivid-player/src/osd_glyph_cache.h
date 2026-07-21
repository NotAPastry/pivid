/* apps/pivid-player/src/osd_glyph_cache.h -- cache rasterised glyph surfaces
 * keyed by an integer key. Rebuilt when the cell size changes. The actual
 * rasterisation is injected as a callback so the cache logic is pure/testable. */
#ifndef PLAYER_OSD_GLYPH_CACHE_H
#define PLAYER_OSD_GLYPH_CACHE_H
#include <cairo.h>
#include <stdint.h>

#define OSD_CACHE_MAX 128

/* Rasterise one glyph into a NEW cairo image surface at the given cell size.
 * Returns NULL on failure. `ud` is the renderer context. */
typedef cairo_surface_t *(*osd_raster_fn)(void *ud, uint32_t key, int cw, int ch);

typedef struct {
    uint32_t         key;
    cairo_surface_t *surf;        /* owned */
} osd_cache_entry;

typedef struct {
    osd_cache_entry entries[OSD_CACHE_MAX];
    int             n;
    int             cw, ch;       /* cell size the cache was built for */
    osd_raster_fn   raster;
    void           *ud;
} OsdGlyphCache;

void osd_glyph_cache_init(OsdGlyphCache *c, osd_raster_fn raster, void *ud);
void osd_glyph_cache_free(OsdGlyphCache *c);

/* Return the cached surface for `key` at cell size (cw,ch), building it on
 * miss. A changed cell size flushes the cache first. May return NULL if the
 * rasteriser fails or the cache is full. Surface stays owned by the cache. */
cairo_surface_t *osd_glyph_cache_get(OsdGlyphCache *c, uint32_t key, int cw, int ch);
#endif
