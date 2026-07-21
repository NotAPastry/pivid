/* apps/pivid-player/src/render.c -- see render.h. */
#include "render.h"
#include "osd_msp_font.h"
#include "osd_grid_render.h"
#include "player_args.h"   /* PLAYER_DEFAULT_WIN_W/H */

#include <SDL.h>
#include <cairo/cairo.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <stdio.h>
#include <string.h>

struct Render {
    SDL_Window     *win;
    SDL_Renderer   *ren;
    SDL_Texture    *video_tex;     /* NV12 or IYUV */
    int             vt_w, vt_h;
    Uint32          vt_fmt;
    SDL_Texture    *osd_tex;       /* ARGB8888, window-sized */
    int             osd_w, osd_h;
    cairo_surface_t*osd_surf;
    cairo_t        *osd_cr;
    const char     *font;          /* toy-API monospace face for the status banner */
    OsdMspFont     *font_msp;
    OsdGridRender  *gr;
    bool            sized_to_video;
    SDL_BlendMode   premul;        /* premultiplied-alpha blend for cairo output */

    /* OSD recomposite cache. Re-rasterizing + re-uploading the window-resolution
     * OSD ARGB texture every frame is ~1 GB/s of memory-bus traffic at 1080p60;
     * on the RX Pi that perturbs the bcm2835 SPI-DMA radio read and corrupts
     * reception for ALL subscribers. So we rebuild the OSD texture only when its
     * inputs actually change (grid content, banner state, blink phase, size). */
    bool            osd_valid;     /* osd_tex holds current content */
    osd_grid_t      osd_last_grid;
    PlayerConnState osd_last_state;
    bool            osd_last_blink;
    bool            osd_has_blink; /* last grid contained any OSD_ATTR_BLINK cell */
};

/* An OS-appropriate monospace face that ships with the platform. On Linux the
 * fontconfig generic "monospace" resolves to the configured default even on
 * headless installs (no specific font package assumed). */
static const char *banner_font_face(void) {
#if defined(__APPLE__)
    return "Menlo";
#elif defined(_WIN32)
    return "Consolas";
#else
    return "monospace";
#endif
}

Render *render_create(int req_w, int req_h, bool fullscreen) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return NULL;
    }
    Render *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->font = banner_font_face();

    int cw = req_w > 0 ? req_w : PLAYER_DEFAULT_WIN_W;
    int ch = req_h > 0 ? req_h : PLAYER_DEFAULT_WIN_H;
    /* Fullscreen WITH an explicit --width/--height switches the CRTC to that
     * (smaller) video mode. On the RX Pi the source is 480p, so scanning out at
     * 480p/720p instead of the panel's native 1080p uses far less memory-bus
     * bandwidth -- which is what contends with the SPI-DMA radio read. Fullscreen
     * with no size given keeps the desktop (native) mode. Windowed otherwise. */
    bool real_fs = fullscreen && req_w > 0 && req_h > 0;
    Uint32 win_flags = fullscreen
        ? (real_fs ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP)
        : SDL_WINDOW_RESIZABLE;
    r->win = SDL_CreateWindow("pivid-player", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, cw, ch, win_flags);
    if (!r->win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); free(r); return NULL; }
    if (real_fs) {
        /* Pin the actual scanout mode to the closest the panel offers. */
        SDL_DisplayMode want = { 0, cw, ch, 0, NULL }, got;
        int di = SDL_GetWindowDisplayIndex(r->win);
        if (di >= 0 && SDL_GetClosestDisplayMode(di, &want, &got))
            SDL_SetWindowDisplayMode(r->win, &got);
        else
            SDL_SetWindowDisplayMode(r->win, &want);
    }
    r->ren = SDL_CreateRenderer(r->win, -1,
                                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r->ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
                   SDL_DestroyWindow(r->win); free(r); return NULL; }
    /* KMSDRM (the RX Pi's console scanout) has no desktop: a mouse cursor
     * parked over the video is pure noise, so hide it there. Windowed
     * desktop builds (macOS/Linux desktop) keep the normal system cursor. */
    {
        const char *vd = SDL_GetCurrentVideoDriver();
        if (vd && strcmp(vd, "KMSDRM") == 0) SDL_ShowCursor(SDL_DISABLE);
    }
    /* Cairo emits premultiplied ARGB32; match it so OSD edges don't fringe. */
    r->premul = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);

    r->font_msp = osd_msp_font_create();
    if (!r->font_msp) fprintf(stderr, "pivid-player: OSD font failed to load\n");
    r->gr = r->font_msp ? osd_grid_render_create(r->font_msp) : NULL;
    return r;
}

void render_destroy(Render *r) {
    if (!r) return;
    if (r->gr) osd_grid_render_destroy(r->gr);
    if (r->font_msp) osd_msp_font_destroy(r->font_msp);
    if (r->osd_cr)   cairo_destroy(r->osd_cr);
    if (r->osd_surf) cairo_surface_destroy(r->osd_surf);
    if (r->osd_tex)  SDL_DestroyTexture(r->osd_tex);
    if (r->video_tex)SDL_DestroyTexture(r->video_tex);
    if (r->ren) SDL_DestroyRenderer(r->ren);
    if (r->win) SDL_DestroyWindow(r->win);
    free(r);
    SDL_Quit();
}

bool render_pump_events(Render *r) {
    (void)r;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_KEYDOWN &&
            (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q)) return false;
    }
    return true;
}

/* (Re)create the OSD surface+texture at window size. */
static void ensure_osd(Render *r, int w, int h) {
    if (r->osd_tex && r->osd_w == w && r->osd_h == h) return;
    if (r->osd_cr)   { cairo_destroy(r->osd_cr); r->osd_cr = NULL; }
    if (r->osd_surf) { cairo_surface_destroy(r->osd_surf); r->osd_surf = NULL; }
    if (r->osd_tex)  { SDL_DestroyTexture(r->osd_tex); r->osd_tex = NULL; }
    r->osd_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    r->osd_cr   = cairo_create(r->osd_surf);
    r->osd_tex  = SDL_CreateTexture(r->ren, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureBlendMode(r->osd_tex, r->premul);
    r->osd_w = w; r->osd_h = h;
    r->osd_valid = false;   /* new texture -> cached content is stale */
}

/* (Re)create the video texture for a frame's size+format. Returns false if the
 * pixel format is unsupported (caller then renders black). */
static bool ensure_video(Render *r, const AVFrame *f) {
    Uint32 fmt;
    if (f->format == AV_PIX_FMT_NV12)        fmt = SDL_PIXELFORMAT_NV12;
    else if (f->format == AV_PIX_FMT_YUV420P)fmt = SDL_PIXELFORMAT_IYUV;
    else return false;   /* uniform path only handles NV12 / I420 (Decision 3) */
    if (r->video_tex && r->vt_w == f->width && r->vt_h == f->height && r->vt_fmt == fmt)
        return true;
    if (r->video_tex) SDL_DestroyTexture(r->video_tex);
    r->video_tex = SDL_CreateTexture(r->ren, fmt, SDL_TEXTUREACCESS_STREAMING,
                                     f->width, f->height);
    r->vt_w = f->width; r->vt_h = f->height; r->vt_fmt = fmt;
    return r->video_tex != NULL;
}

static void upload_video(Render *r, const AVFrame *f) {
    if (r->vt_fmt == SDL_PIXELFORMAT_NV12)
        SDL_UpdateNVTexture(r->video_tex, NULL,
                            f->data[0], f->linesize[0],
                            f->data[1], f->linesize[1]);
    else  /* IYUV */
        SDL_UpdateYUVTexture(r->video_tex, NULL,
                             f->data[0], f->linesize[0],
                             f->data[1], f->linesize[1],
                             f->data[2], f->linesize[2]);
}

static void draw_banner(Render *r, int w, int h, const char *text) {
    /* Centered status line, drawn into the OSD cairo surface (reuses its texture).
     * Simple: small white text on a translucent strip. */
    cairo_t *cr = r->osd_cr;
    cairo_select_font_face(cr, r->font, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    double sz = h / 28.0;
    cairo_set_font_size(cr, sz);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    double x = (w - ext.width) / 2.0, y = h * 0.62;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_rectangle(cr, x - sz*0.5, y - sz, ext.width + sz, sz*1.5);
    cairo_fill(cr);
    cairo_move_to(cr, x, y);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_show_text(cr, text);
}

void render_present(Render *r, const AVFrame *frame,
                    const osd_grid_t *grid, PlayerConnState state,
                    int64_t now_ns, bool video_changed) {
    /* First-frame window sizing (Decision 2: no TXT presizing in v1). */
    if (frame && !r->sized_to_video) {
        SDL_SetWindowSize(r->win, frame->width, frame->height);
        r->sized_to_video = true;
    }
    int win_w, win_h;
    SDL_GetRendererOutputSize(r->ren, &win_w, &win_h);

    /* Decide whether anything changed before touching the GPU. */
    ensure_osd(r, win_w, win_h);   /* may set osd_valid=false on a size change */
    bool blink = BLINK_ON(now_ns);
    bool osd_dirty = !r->osd_valid
        || state != r->osd_last_state
        || (r->osd_has_blink && blink != r->osd_last_blink)
        || memcmp(grid, &r->osd_last_grid, sizeof *grid) != 0;

    /* Nothing new to show -> skip the GPU clear/composite/present entirely. The
     * last presented frame stays on screen. This bounds display traffic to the
     * actual content rate (video ~30 Hz, OSD ~5 Hz) instead of the loop rate. */
    if (!video_changed && !osd_dirty) return;

    SDL_SetRenderDrawColor(r->ren, 0, 0, 0, 255);
    SDL_RenderClear(r->ren);

    /* Video (scaled to fill the window) or black. */
    if (frame && ensure_video(r, frame)) {
        if (video_changed) upload_video(r, frame);
        SDL_RenderCopy(r->ren, r->video_tex, NULL, NULL);
    }

    /* OSD at WINDOW resolution (crisp text when 480p is upscaled). Only
     * re-rasterize + re-upload when the composited result actually changes --
     * see the osd cache fields. Drawing it every frame is a ~1 GB/s memory-bus
     * hog at 1080p60 that corrupts the radio reception on the RX Pi. */
    if (osd_dirty) {
        cairo_save(r->osd_cr);
        cairo_set_operator(r->osd_cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(r->osd_cr);
        cairo_restore(r->osd_cr);

        if (r->gr)
            osd_grid_render_draw(r->gr, r->osd_cr, grid, win_w, win_h, now_ns);

        const char *banner = player_state_banner(state);
        if (banner) draw_banner(r, win_w, win_h, banner);

        cairo_surface_flush(r->osd_surf);
        SDL_UpdateTexture(r->osd_tex, NULL,
                          cairo_image_surface_get_data(r->osd_surf),
                          cairo_image_surface_get_stride(r->osd_surf));

        /* Remember the inputs this texture was built from. */
        r->osd_last_grid  = *grid;
        r->osd_last_state = state;
        r->osd_last_blink = blink;
        r->osd_has_blink  = false;
        for (unsigned i = 0; i < OSD_GRID_CELLS; i++)
            if (grid->cells[i].attr & OSD_ATTR_BLINK) { r->osd_has_blink = true; break; }
        r->osd_valid = true;
    }

    SDL_RenderCopy(r->ren, r->osd_tex, NULL, NULL);

    SDL_RenderPresent(r->ren);
}
