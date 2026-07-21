#ifndef TX_CAMERA_SPAWN_H
#define TX_CAMERA_SPAWN_H

#include "settings.h"
#include <sys/types.h>

/* Owns the rpicam-vid child process and the camera pipe. The parent reads
 * H.264 Annex-B from read_fd (which it feeds to the encoder in place of stdin).
 * Single-threaded use from the main thread only (PR_SET_PDEATHSIG tracks the
 * forking thread, so the fork must come from the thread that lives the whole
 * process). */
typedef struct {
    pid_t    pid;            /* child pid, or -1 when not running             */
    int      read_fd;        /* parent read end of the camera pipe, or -1     */
    int      backoff_ms;     /* current respawn backoff (bounded), 0 at start */
    char     bin[256];       /* resolved rpicam-vid path (PATH or override)   */
    Settings settings;       /* snapshot, for rebuilding argv on respawn      */
} CameraSpawn;

/* Initial backoff and ceiling for respawn (bounded). */
#define CAMERA_BACKOFF_MS_MIN  200
#define CAMERA_BACKOFF_MS_MAX  2000

/* Resolve the binary (PIVID_RPICAM_BIN override, else "rpicam-vid" on PATH),
 * snapshot `s`, build the argv, create the pipe, and fork/exec. On success sets
 * cs->pid and cs->read_fd (O_CLOEXEC, best-effort enlarged) and returns 0.
 * Returns -1 on a fatal local failure (bad recipe / pipe / fork). An execvp
 * failure in the child is NOT a -1 here: the child prints a clear error and
 * exits non-zero, which the parent observes as immediate EOF on read_fd plus a
 * non-zero status from camera_spawn_reaped(). */
int camera_spawn_start(CameraSpawn *cs, const Settings *s);

/* After EOF on read_fd, reap the child with waitpid(WNOHANG) and log its exit
 * status. Returns 1 if reaped (caller should respawn), 0 if still alive (rare
 * spurious wake), -1 on waitpid error. Sets cs->pid = -1 when reaped. Does NOT
 * touch read_fd. */
int camera_spawn_reaped(CameraSpawn *cs);

/* Close the (dead) read_fd, sleep the bounded backoff, and start a fresh child
 * from cs->settings. Updates cs->read_fd to the new pipe and grows cs->backoff_ms
 * toward CAMERA_BACKOFF_MS_MAX. Returns 0 (new child running) or -1 (fatal).
 * The backoff nanosleep is interruptible (EINTR returns early) so a shutdown
 * signal during backoff is acted on promptly by the caller. */
int camera_spawn_respawn(CameraSpawn *cs);

/* Graceful teardown, split so the child's exit latency overlaps the daemon's
 * existing teardown rather than blocking in the middle of it:
 *
 *  - camera_spawn_terminate(): send SIGTERM to the child. Call at the TOP of
 *    teardown (no-op if already gone). libcamera then releases the media/V4L2
 *    device cleanly.
 *  - camera_spawn_finish(): call at the BOTTOM of teardown. Bounded-wait for the
 *    child (WNOHANG + nanosleep up to deadline_ms), escalate to SIGKILL on
 *    overrun, reap, and ONLY THEN close read_fd (last, so a still-flushing child
 *    gets EOF/clean-exit, not SIGPIPE). Idempotent / safe if pid already -1. */
void camera_spawn_terminate(CameraSpawn *cs);
void camera_spawn_finish(CameraSpawn *cs, int deadline_ms);

#endif /* TX_CAMERA_SPAWN_H */
