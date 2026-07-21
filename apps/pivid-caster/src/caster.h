/* apps/pivid-caster/src/caster.h -- the caster event loop (no avahi).
 *
 * Binds video-in 127.0.0.1:LOOPBACK_VIDEO_PORT, telem-in
 * 127.0.0.1:LOOPBACK_TELEM_PORT (both from common/loopback.h, fed by
 * pivid-rx) and control 0.0.0.0:CASTER_CTRL_PORT. Each `caster_step` does one
 * poll() iteration: a control datagram is src-gated, parsed, and upserted/
 * removed; a video/telem datagram is fanned out verbatim by sendto to every
 * live subscriber's port. Stale subscribers are aged out each step. */
#ifndef CASTER_CASTER_H
#define CASTER_CASTER_H
#include <stdbool.h>
#include <stdint.h>
#include "subscribers.h"

typedef struct {
    int       video_fd;     /* bound 127.0.0.1:5600 (ingest from rx)            */
    int       telem_fd;     /* bound 127.0.0.1:5601 (ingest from rx)            */
    int       ctrl_fd;      /* bound 0.0.0.0:5700 (control)                     */
    int       fwd_fd;       /* UNBOUND egress socket for fan-out (see caster.c) */
    SubTable  subs;
    bool      forward_enabled;
    uint64_t  video_in, telem_in;        /* datagrams received from rx          */
    /* video_fwd counts per-subscriber CHUNK sends (each NAL fans out as
     * ceil(len/1400) lanfrag chunks), NOT whole NALs -- so it is ~tens x
     * video_in, not comparable 1:1. telem_fwd stays one datagram per subscriber. */
    uint64_t  video_fwd, telem_fwd;      /* video: chunks sent; telem: datagrams */
    uint64_t  bytes_fwd;                 /* wire bytes forwarded (incl. 8B/chunk hdr) */
    uint64_t  subs_rejected;             /* over-cap SUBSCRIBEs refused          */
    uint64_t  ctrl_dropped;              /* gated/garbage control datagrams      */
    uint16_t  vid_nal_id;                /* rolling per-NAL id stamped on chunks  */
    uint64_t  oversize_drops;            /* video NALs > LANFRAG_MAX_NAL, dropped  */
    uint64_t  chunk_send_fail;           /* per-chunk sendmsg short/failed writes  */
    uint64_t  telem_send_fail;           /* per-subscriber telem sendto short/fail  */
} CasterCtx;

/* Open + bind the three sockets. Returns 0 on success, -1 on any socket/bind
 * failure (everything opened so far is closed on failure). */
int  caster_open(CasterCtx *c, bool forward_enabled);

/* One poll() iteration (up to `timeout_ms`), then an age-out pass using
 * `now_ms`. Services whichever sockets are readable. Returns the number of fds
 * serviced (>=0), or -1 on a poll() error (EINTR returns 0). */
int  caster_step(CasterCtx *c, int timeout_ms, int64_t now_ms);

/* Flip the forwarding gate live (SIGHUP path). */
void caster_set_forward(CasterCtx *c, bool enabled);

/* Close all sockets. */
void caster_close(CasterCtx *c);

#ifdef LATPROBE
/* Dump the S6b caster-forward residency histogram to `fd` (SIGUSR1 path). */
void caster_probe_dump(int fd);
#endif

#endif
