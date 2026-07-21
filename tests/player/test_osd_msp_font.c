#include "osd_msp_font.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Write a small ARGB surface to a PNG in memory, returning malloc'd buffer. */
struct buf { unsigned char *p; unsigned len, cap; };
static cairo_status_t wr(void *c, const unsigned char *data, unsigned int n){
    struct buf *b=c; if(b->len+n>b->cap){ b->cap=(b->len+n)*2; b->p=realloc(b->p,b->cap);}
    memcpy(b->p+b->len,data,n); b->len+=n; return CAIRO_STATUS_SUCCESS;
}
int main(void){
    /* synthetic "font": 36 wide x (54*3) tall = 3 glyphs */
    cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,OSD_FONT_GW,OSD_FONT_GH*3);
    cairo_t *cr=cairo_create(s); cairo_set_source_rgba(cr,1,1,1,1); cairo_paint(cr); cairo_destroy(cr);
    struct buf b={0}; cairo_surface_write_to_png_stream(s,wr,&b); cairo_surface_destroy(s);

    OsdMspFont *f=osd_msp_font_create_from_png(b.p,b.len);
    assert(f);
    cairo_surface_t *sh=osd_msp_font_sheet(f);
    assert(cairo_image_surface_get_width(sh)==OSD_FONT_GW);
    assert(cairo_image_surface_get_height(sh)==OSD_FONT_GH*3);
    double sx,sy; osd_msp_font_src(2,&sx,&sy);
    assert(sx==0.0 && sy==(double)(2*OSD_FONT_GH));
    /* garbage bytes -> NULL, not crash */
    unsigned char junk[8]={1,2,3,4,5,6,7,8};
    assert(osd_msp_font_create_from_png(junk,sizeof junk)==NULL);
    osd_msp_font_destroy(f); free(b.p);
    printf("test_osd_msp_font: OK\n");
    return 0;
}
