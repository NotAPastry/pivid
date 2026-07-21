/* apps/pivid-player/src/video_decode.c -- see video_decode.h.
 *
 * Decision 3: per-platform HW device preference + software fallback; HW frames
 * are pulled to system memory with av_hwframe_transfer_data so the render path
 * is uniform (planar NV12/IYUV -> SDL YUV texture; no sws_scale). */
#define _POSIX_C_SOURCE 200809L
#include "video_decode.h"
#include "lan_source.h"
#include "player_clock.h"

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LATPROBE
#include "resid_hist.h"
#include "mono_ns.h"
static ResidHist g_s7_decode;
static const uint64_t S7_EDGES[] = {
    100000, 250000, 500000, 1000000, 2000000, 4000000,
    8000000, 16000000, 24000000, 33000000, (uint64_t)-1 };
void video_decode_probe_dump(int fd) { resid_hist_dump(&g_s7_decode, fd); }
#endif

/* Auto-acquire fallback: if the SW decoder produces only non-key frames for this
 * long after the first decoded frame, the stream has no IDR (a gdr intra-refresh
 * recipe) -- start showing the heal-in rather than hang. Must exceed the largest
 * IDR interval any IDR recipe uses (gdr_idr default 60f = 2 s) so those acquire
 * cleanly at their IDR instead of tripping this. 2.5 s = 2 s + margin. */
#define SHOWALL_FALLBACK_NS  2500000000LL

/* Preferred HW device for this platform, or AV_HWDEVICE_TYPE_NONE.
 *
 * Default is SOFTWARE decode. On a damaged picture the platform HW decoders
 * (VideoToolbox/D3D11VA/VAAPI) tear down and rebuild the whole decode session
 * (libav logs "VideoToolbox decoder needs reconfig, restarting.."), which then
 * emits nothing until the next IDR (~1 s at the TX's 1 IDR/s cadence). The live
 * RF recipe uses FEC-unprotected droppable P-frames, so individual frames are
 * damaged far more often than that -- HW decode lives in a perpetual
 * teardown/reacquire cycle (black window), worsened by UDP socket overflow
 * dropping the recovery IDRs while the session is restarting. The software
 * decoder conceals each damaged frame in place, never tears down, and stays
 * locked; at 480p30 it is cheap. (Measured on a real 12 s live capture: SW
 * 361/361 frames, 0 errors; VideoToolbox 3 session restarts on the same bytes.)
 *
 * So software is the default; --hw opts into hardware for clean streams. */
static enum AVHWDeviceType preferred_hw(bool want_hw) {
    if (!want_hw) return AV_HWDEVICE_TYPE_NONE;   /* software */
#if defined(__APPLE__)
    return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(_WIN32)
    return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__linux__)
    return AV_HWDEVICE_TYPE_VAAPI;
#else
    return AV_HWDEVICE_TYPE_NONE;
#endif
}

/* The HW pixel format this decoder will emit, chosen by get_format. */
static enum AVPixelFormat g_hw_pix_fmt = AV_PIX_FMT_NONE;

static enum AVPixelFormat pick_format(AVCodecContext *ctx,
                                      const enum AVPixelFormat *fmts) {
    (void)ctx;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == g_hw_pix_fmt) return *p;
    /* HW format not offered -> let avcodec pick (software). */
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        return *p;
    return AV_PIX_FMT_NONE;
}

/* Concealment/low-latency decode policy, shared with the fecsim metrics harness
 * so its PSNR reflects the SAME in-place concealment the player uses. Apply to an
 * allocated-but-not-yet-avcodec_open2'd context. err_recognition is a bitfield; 0
 * clears AV_EF_EXPLODE (abort-on-error) and all strict checks -- the right move
 * for maximum concealment on the lossy droppable-P link. */
/* video_decode_open_flags() lives in video_decode_flags.c (its own TU) so tools
 * can link the shared concealment policy without the threaded decode path. */

static void *run(void *arg) {
    VideoDecode *v = (VideoDecode *)arg;
    AVCodecContext *cc = NULL;
    AVBufferRef    *hw = NULL;

    const AVCodec *dec = NULL;
    bool use_v4l2m2m = false;
#if defined(__linux__)
    /* On the Pi the HW H.264 decoder is V4L2 M2M (bcm2835-codec), NOT VAAPI.
     * When --hw is requested, select it by name; it offloads decode off the ARM
     * cores (and its memory-access pattern eases the SPI-DMA radio contention on
     * the RX Pi). Falls through to the SW decoder if unavailable. */
    if (v->hw_decode) {
        dec = avcodec_find_decoder_by_name("h264_v4l2m2m");
        if (dec) { atomic_store(&v->using_hw, true); use_v4l2m2m = true;
                   fprintf(stderr, "video_decode: using h264_v4l2m2m (V4L2 M2M HW)\n"); }
    }
#endif
    /* Decode AUs directly (from lan_source's au_asm) -- no libavformat: avoids
     * the demuxer's next-AU parser hold (the ~1-frame latency this whole change
     * removes) and its join-time probe race. */
    if (!dec) dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!dec) { fprintf(stderr, "video_decode: no H.264 decoder\n"); return NULL; }

    cc = avcodec_alloc_context3(dec);
    if (!cc) { fprintf(stderr, "video_decode: alloc ctx failed\n"); return NULL; }
    cc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    /* No extradata: SPS/PPS are in-band in the AU stream; the decoder inits
     * lazily on the first SPS/PPS/IDR AU. */
    /* Software path: always decode through pre-keyframe so a late joiner never
     * stalls; the publish gate below decides when to actually show frames. HW
     * decoders (opt-in, clean streams) keep the wait-for-keyframe behaviour --
     * showall would just feed their teardown/rebuild cycle on damaged pictures.
     * Conceal in place (video_decode_open_flags): emit corrupt/concealed
     * pictures instead of dropping them, and disable strict error checks. Shared
     * with fs_metrics so PSNR reflects the same concealment. Applied to SW AND
     * the Pi's V4L2 M2M decoder (it conceals damaged slices in place rather than
     * erroring/freezing -- closer to the Mac SW picture under loss). */
    if (!v->hw_decode || use_v4l2m2m) video_decode_open_flags(cc);
    if (!v->hw_decode) {
        /* SW decode memory traffic contends with the SPI-DMA radio read on the
         * RX Pi. Capping decode threads lowers the peak bandwidth. Tunable for
         * A/B (PIVID_DECODE_THREADS); default unset = libavcodec auto (per-core). */
        const char *nt = getenv("PIVID_DECODE_THREADS");
        if (nt && *nt) cc->thread_count = atoi(nt);
        /* Slice threading (one frame across all cores) instead of the default
         * frame threading (several frames in parallel). Frame threading adds
         * latency and delivers frames in bursts -> on the weak Zero 2 W the
         * per-frame decode-time variance (esp. the periodic I-frame) shows as a
         * choppy hitch. Slice threading gives uniform, low-latency delivery. */
        cc->thread_type = FF_THREAD_SLICE;
        const char *tt = getenv("PIVID_DECODE_THREAD_TYPE");   /* "frame" to A/B */
        if (tt && !strcmp(tt, "frame")) cc->thread_type = FF_THREAD_FRAME;
    }

    /* Try HW device; on any failure, fall through to software. */
    enum AVHWDeviceType hwtype = preferred_hw(v->hw_decode);
    if (hwtype != AV_HWDEVICE_TYPE_NONE) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(dec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == hwtype) {
                if (av_hwdevice_ctx_create(&hw, hwtype, NULL, NULL, 0) == 0) {
                    g_hw_pix_fmt = cfg->pix_fmt;
                    cc->hw_device_ctx = av_buffer_ref(hw);
                    cc->get_format = pick_format;
                    atomic_store(&v->using_hw, true);
                }
                break;
            }
        }
    }
    if (avcodec_open2(cc, dec, NULL) < 0) {
        fprintf(stderr, "video_decode: avcodec_open2 failed\n");
        goto done;
    }
    fprintf(stderr, "video_decode: %s decode\n",
            atomic_load(&v->using_hw) ? "hardware" : "software");

    AVPacket *pkt   = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();   /* decoder output (maybe HW) */
    AVFrame  *sw    = av_frame_alloc();   /* system-memory copy        */

#ifdef LATPROBE
    resid_hist_init(&g_s7_decode, "s7_decode", S7_EDGES,
                    (int)(sizeof S7_EDGES / sizeof S7_EDGES[0]), 2000000ull);
#endif

    while (!atomic_load(&v->stop)) {
        const uint8_t *au; size_t au_len;
        if (lan_source_next_au(&v->lan_src, &au, &au_len) == LAN_SOURCE_STOP) break;
#ifdef LATPROBE
        uint64_t au_ing = lan_source_cur_ingress_ns(&v->lan_src);
#endif
        if (au_len == 0) continue;
        if (av_new_packet(pkt, (int)au_len) < 0) continue;
        /* au points into lan_source's AU ring, valid only until the next
         * lan_source_next_au() -- copy now. */
        memcpy(pkt->data, au, au_len);
        int sret = avcodec_send_packet(cc, pkt);
        av_packet_unref(pkt);
        if (sret < 0) continue;

        while (avcodec_receive_frame(cc, frame) == 0) {
#ifdef LATPROBE
            /* Attributes this AU's ingress to the frame the decode loop emits --
             * exact on the no-B-frame baseline (~1:1 AU->frame). */
            if (au_ing) {
                uint64_t now = pv_mono_ns();
                if (now >= au_ing) resid_hist_add(&g_s7_decode, now - au_ing);
            }
#endif
            AVFrame *out = frame;
            if (frame->format == g_hw_pix_fmt && g_hw_pix_fmt != AV_PIX_FMT_NONE) {
                av_frame_unref(sw);
                if (av_hwframe_transfer_data(sw, frame, 0) == 0) {
                    av_frame_copy_props(sw, frame);
                    out = sw;               /* system-memory NV12 */
                } else {
                    av_frame_unref(frame);
                    continue;               /* drop on transfer failure */
                }
            }
            /* Auto-acquire gate. Decode-through is on (SW), so frames arrive
             * even mid-GOP with no reference -- garbage. Withhold them until the
             * stream proves an entry point: publish on the first KEYFRAME (clean
             * IDR acquire for droppable/gop/gdr_idr), or, if only non-key frames
             * keep coming past the max IDR interval, conclude it's a gdr
             * intra-refresh stream with no IDR and show the heal-in. Latch once.
             * (HW path emits nothing pre-keyframe, so the first frame here is the
             * keyframe and the gate is a no-op.) */
            if (!v->acquired) {
                int64_t t = now_ns();
                if (v->first_decode_ns == 0) v->first_decode_ns = t;
                bool key = (out->flags & AV_FRAME_FLAG_KEY) != 0;
                bool timed_out = (t - v->first_decode_ns) >= SHOWALL_FALLBACK_NS;
                if (!key && !timed_out) { av_frame_unref(frame); continue; }
                v->acquired = true;
                if (!key)
                    fprintf(stderr, "video_decode: no IDR after %.1fs -- intra-refresh "
                            "stream, showing heal-in\n", SHOWALL_FALLBACK_NS / 1e9);
            }
            frame_slot_publish(v->slot, out, now_ns());
            atomic_fetch_add(&v->frames_decoded, 1);
            av_frame_unref(frame);
        }
    }

    av_frame_free(&sw);
    av_frame_free(&frame);
    av_packet_free(&pkt);
done:
    if (hw) av_buffer_unref(&hw);
    avcodec_free_context(&cc);
    return NULL;
}

int video_decode_start(VideoDecode *v, uint16_t video_port, FrameSlot *slot,
                       bool hw_decode) {
    memset(v, 0, sizeof *v);   /* zeroes acquired + first_decode_ns; stop=false */
    /* video_port == 0 => bind an ephemeral port so the player needs no port
     * config; the actual port is advertised to the caster via SUBSCRIBE. We bind
     * (and KEEP) our own UDP socket here, so there is no close/rebind race. stop
     * is already false (memset); lan_source_open only stores its address. */
    uint16_t bound = 0;
    if (lan_source_open(&v->lan_src, video_port, &v->stop, &bound) != 0 || bound == 0) {
        fprintf(stderr, "video_decode: could not acquire a UDP port\n");
        return -1;
    }
    v->video_port = bound;
    v->slot       = slot;
    v->hw_decode  = hw_decode;
    atomic_store(&v->stop, false);
    atomic_store(&v->frames_decoded, 0);
    atomic_store(&v->using_hw, false);
    if (pthread_create(&v->thread, NULL, run, v) != 0) {
        lan_source_close(&v->lan_src);   /* no thread to use the socket; reclaim it */
        return -1;
    }
    return 0;
}

void video_decode_stop(VideoDecode *v) {
    atomic_store(&v->stop, true);
    pthread_join(v->thread, NULL);
    lan_source_close(&v->lan_src);   /* only after the decode thread releases the fd */
}
