/* apps/pivid-player/src/video_decode_flags.c
 *
 * The shared codec-open concealment policy, in its own translation unit so it
 * can be linked by an offline metrics harness WITHOUT dragging in the
 * player's threaded decode path (lan_source/frame_slot/pthreads). The player
 * links this TU alongside video_decode.c; fs_metrics links only this one.
 * Single source of truth for the decode flags -> byte-identical concealment in
 * the live player and in offline PSNR measurement. */
#define _POSIX_C_SOURCE 200809L
#include "video_decode.h"

#include <libavcodec/avcodec.h>

void video_decode_open_flags(struct AVCodecContext *cc) {
    cc->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    cc->flags  |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    cc->err_recognition = 0;
    cc->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
}
