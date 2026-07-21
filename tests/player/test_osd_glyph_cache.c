#include "osd_glyph_cache.h"
#include <assert.h>
#include <stdio.h>

static int raster_calls = 0;
static cairo_surface_t *stub(void *ud, uint32_t key, int cw, int ch) {
    (void)ud; (void)key;
    raster_calls++;
    return cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
}

int main(void) {
    OsdGlyphCache c; osd_glyph_cache_init(&c, stub, NULL);

    cairo_surface_t *a = osd_glyph_cache_get(&c, 'A', 20, 30);
    assert(a && raster_calls == 1);
    /* same key + size -> cached, no new raster */
    cairo_surface_t *a2 = osd_glyph_cache_get(&c, 'A', 20, 30);
    assert(a2 == a && raster_calls == 1);
    /* different key -> new raster */
    osd_glyph_cache_get(&c, 'B', 20, 30);
    assert(raster_calls == 2);
    /* size change -> flush + rebuild (don't assert pointer identity: cairo may
     * reuse the freed address; assert the observable effects instead) */
    cairo_surface_t *a3 = osd_glyph_cache_get(&c, 'A', 40, 60);
    assert(a3 != NULL);            /* rasteriser succeeded */
    assert(raster_calls == 3);     /* flush forced a re-rasterise */
    assert(c.n == 1);              /* old entries were cleared */

    /* distinct key -> separate entry + new raster */
    osd_glyph_cache_get(&c, 'B', 40, 60);
    assert(raster_calls == 4 && c.n == 2);

    osd_glyph_cache_free(&c);
    printf("test_osd_glyph_cache: OK\n");
    return 0;
}
