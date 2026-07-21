/* apps/pivid-player/src/video_decode.h -- libavformat udp:// ingest + libavcodec
 * HW decode thread. Opens udp://@:PORT with low-latency flags, pulls Annex-B
 * packets, HW-decodes (VideoToolbox/D3D11VA/VAAPI; software fallback) and
 * publishes each frame to a FrameSlot. */
#ifndef PLAYER_VIDEO_DECODE_H
#define PLAYER_VIDEO_DECODE_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "frame_slot.h"
#include "lan_source.h"

struct AVCodecContext;
/* Apply the player's concealment/low-latency decode policy to an opened-but-not-
 * yet-avcodec_open2'd context: LOW_DELAY|OUTPUT_CORRUPT, err_recognition=0,
 * FLAG2_SHOW_ALL. Shared with the fecsim metrics harness so PSNR reflects the
 * SAME in-place concealment the player uses. */
void video_decode_open_flags(struct AVCodecContext *cc);

typedef struct {
    uint16_t      video_port;
    FrameSlot    *slot;
    bool          hw_decode;       /* resolved decode mode: true=HW, false=SW */
    atomic_bool   stop;
    pthread_t     thread;
    LanSource     lan_src;         /* UDP source feeding the custom AVIOContext */
    /* Auto-acquire gate (decode-thread-private; no atomics needed). The SW
     * decoder opens with AV_CODEC_FLAG2_SHOW_ALL so it never stalls pre-keyframe;
     * frames are decoded but withheld until the stream proves it has a decode
     * entry point. See run() for the keyframe-or-timeout logic. */
    bool          acquired;        /* latched true on first published frame */
    int64_t       first_decode_ns; /* when the first frame came out of avcodec */
    /* observability counters (atomic; reserved for a window-title/log readout) */
    atomic_ullong frames_decoded;
    atomic_bool   using_hw;        /* true once a HW decoder is in use */
} VideoDecode;

/* Spawn the decode thread. hw_decode selects the platform HW decoder (true) or
 * software (false); the caller resolves the tri-state. Late-join behaviour is
 * automatic (no flag): the SW decoder always decodes through pre-keyframe and
 * the thread auto-detects whether the stream needs it (a `gdr` intra-refresh
 * stream with no IDR) vs should wait for a clean IDR (droppable/gop/gdr_idr).
 * Pass video_port == 0 to bind an ephemeral port; on success v->video_port holds
 * the actually-acquired port (advertise it to the caster).
 * Returns 0 on success, -1 on port/thread-create failure. */
int  video_decode_start(VideoDecode *v, uint16_t video_port, FrameSlot *slot,
                        bool hw_decode);

/* Signal stop and join. */
void video_decode_stop(VideoDecode *v);

#ifdef LATPROBE
/* Dump the S7 decode-residency histogram (piggybacks on the SIGUSR1 stats dump). */
void video_decode_probe_dump(int fd);
#endif

#endif
