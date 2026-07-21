/* apps/pivid-player/src/osd_msp_font.h -- the embedded msp-osd SD font sheet
 * (BTFL_DJI: 144x13824 = 4 pages x 36w x 256 glyphs x 54h; page 0 = white). */
#ifndef PLAYER_OSD_MSP_FONT_H
#define PLAYER_OSD_MSP_FONT_H
#include <cairo.h>
#define OSD_FONT_GW 36
#define OSD_FONT_GH 54
typedef struct OsdMspFont OsdMspFont;   /* owns the sheet surface */

/* Build from the compiled-in font PNG. NULL on failure. */
OsdMspFont *osd_msp_font_create(void);
/* Build from an in-memory PNG (used by tests). NULL on failure. */
OsdMspFont *osd_msp_font_create_from_png(const unsigned char *buf, unsigned len);
void        osd_msp_font_destroy(OsdMspFont *f);

cairo_surface_t *osd_msp_font_sheet(const OsdMspFont *f);   /* not owned */
/* Page-0 source origin (px) of glyph `index` (0..255) within the sheet. */
void osd_msp_font_src(unsigned index, double *sx, double *sy);
#endif
