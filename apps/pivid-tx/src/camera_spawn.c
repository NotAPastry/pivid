/* rpicam-vid spawn/monitor wrapper for pivid-tx. Forks the camera, owns the
 * pipe, detects death via EOF on the pipe (the encoder's poll loop wakes on it),
 * reaps + respawns with bounded backoff. Only g_shutdown exits the daemon (see
 * main.c). Graceful SIGTERM->SIGKILL teardown lives in camera_spawn_terminate/
 * _finish. Linux-only (F_SETPIPE_SZ / PR_SET_PDEATHSIG). */

#define _GNU_SOURCE
#include "camera_spawn.h"
#include "camera_argv.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Best-effort target read-end pipe size (scheduler-jitter slack). Above
 * /proc/sys/fs/pipe-max-size (1 MB default) without CAP_SYS_RESOURCE this
 * EPERMs -- we then keep the default size. Never fatal. */
#define CAMERA_PIPE_SZ  (1 << 20)

/* Resolve argv[0]: PIVID_RPICAM_BIN override (testing / non-standard installs)
 * else the bare "rpicam-vid" (found on systemd's service PATH, where deploy.sh
 * installs the patched binary over /usr/bin/rpicam-vid). */
static void resolve_bin(CameraSpawn *cs) {
    const char *env = getenv("PIVID_RPICAM_BIN");
    snprintf(cs->bin, sizeof cs->bin, "%s", (env && *env) ? env : "rpicam-vid");
}

/* pipe + fork + exec using cs->settings and cs->bin. Sets cs->pid + cs->read_fd.
 * Returns 0, or -1 on bad recipe / pipe / fork. */
static int spawn_once(CameraSpawn *cs) {
    CameraArgv cav;
    if (camera_build_argv(&cs->settings, cs->bin, &cav) != 0) {
        fprintf(stderr, "pivid-tx: camera argv build failed (recipe '%s')\n",
                cs->settings.recipe);
        return -1;
    }

    /* pipe2(O_CLOEXEC): both ends are CLOEXEC atomically (no leak window if
     * another thread forks between pipe() and a later fcntl). The child's
     * dup2(fds[1], STDOUT) makes a fresh, NON-CLOEXEC stdout so rpicam's output
     * survives exec; the child still closes both raw ends before execvp. */
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        fprintf(stderr, "pivid-tx: camera pipe2() failed: %s\n", strerror(errno));
        return -1;
    }

    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "pivid-tx: camera fork() failed: %s\n", strerror(errno));
        close(fds[0]); close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        /* ----- child ----- */
        /* Orphan guard (abnormal-death net): kill us if pivid-tx dies via
         * SIGKILL/SIGSEGV before it can SIGTERM us. PR_SET_PDEATHSIG tracks the
         * forking THREAD -- main.c forks from the main thread, which lives for
         * the whole process. Re-check getppid(): if the parent already died in
         * the fork->prctl window the signal never armed, so exit now. */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(127);

        /* Reset dispositions the parent customised. SIGPIPE in particular: the
         * parent SIG_IGNs it (preserved across exec), but the child must take
         * the default (terminate) so a closed read-end ends rpicam cleanly. */
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGUSR1, SIG_DFL);

        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[0]);
        close(fds[1]);
        execvp(cs->bin, cav.argv);
        /* exec failed: clear, attributable error -- never a silent death. */
        fprintf(stderr, "pivid-tx: exec rpicam-vid ('%s') failed: %s\n",
                cs->bin, strerror(errno));
        _exit(127);
    }

    /* ----- parent ----- */
    close(fds[1]);                          /* drop write-end: else no EOF on child death */
    /* read-end is already CLOEXEC from pipe2 */
    if (fcntl(fds[0], F_SETPIPE_SZ, CAMERA_PIPE_SZ) < 0) {
        fprintf(stderr, "pivid-tx: F_SETPIPE_SZ(%d) failed (%s); "
                        "using default pipe size\n", CAMERA_PIPE_SZ, strerror(errno));
    }
    cs->pid     = pid;
    cs->read_fd = fds[0];
    fprintf(stderr, "pivid-tx: spawned %s pid=%d\n", cs->bin, (int)pid);
    return 0;
}

int camera_spawn_start(CameraSpawn *cs, const Settings *s) {
    if (!cs || !s) return -1;
    memset(cs, 0, sizeof *cs);
    cs->pid     = -1;
    cs->read_fd = -1;
    cs->settings = *s;
    resolve_bin(cs);
    return spawn_once(cs);
}

int camera_spawn_reaped(CameraSpawn *cs) {
    if (!cs || cs->pid <= 0) return -1;
    int status = 0;
    pid_t r = waitpid(cs->pid, &status, WNOHANG);
    if (r == 0)  return 0;                   /* still alive (spurious wake) */
    /* waitpid error (e.g. ECHILD -- effectively unreachable given the pid<=0
     * guard + single-threaded use): treat the child as gone so the caller
     * stops waiting on it. */
    if (r < 0)   { return -1; }
    if (WIFEXITED(status))
        fprintf(stderr, "pivid-tx: rpicam-vid pid=%d exited status=%d\n",
                (int)cs->pid, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        fprintf(stderr, "pivid-tx: rpicam-vid pid=%d killed by signal=%d\n",
                (int)cs->pid, WTERMSIG(status));
    cs->pid = -1;
    return 1;
}

/* Interruptible sleep: returns on completion OR on EINTR (so a shutdown signal
 * during backoff is acted on promptly by the caller's g_shutdown re-check). */
static void sleep_ms_interruptible(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    (void)nanosleep(&ts, NULL);
}

/* GUARANTEE the current child (cs->pid) is reaped, then clear cs->pid. Bounded
 * poll of waitpid(WNOHANG) in step_ms slices up to deadline_ms; if it overruns
 * (child wedged), escalate to SIGKILL + a blocking waitpid so we NEVER block
 * unboundedly and NEVER leak a zombie. Does NOT touch read_fd -- callers own its
 * close ordering. No-op when pid<=0. Shared by camera_spawn_respawn() (reaps the
 * old, dying child before spawn_once overwrites cs->pid) and camera_spawn_finish()
 * (teardown). */
static void reap_child_bounded(CameraSpawn *cs, int deadline_ms) {
    if (!cs || cs->pid <= 0) return;
    const int step_ms = 20;
    int waited = 0, reaped = 0;
    for (;;) {
        pid_t r = waitpid(cs->pid, NULL, WNOHANG);
        if (r == cs->pid || (r < 0 && errno == ECHILD)) { reaped = 1; break; }
        if (waited >= deadline_ms) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)step_ms * 1000000L };
        (void)nanosleep(&ts, NULL);
        waited += step_ms;
    }
    if (!reaped) {
        fprintf(stderr, "pivid-tx: rpicam-vid pid=%d overran %d ms; SIGKILL\n",
                (int)cs->pid, deadline_ms);
        kill(cs->pid, SIGKILL);
        (void)waitpid(cs->pid, NULL, 0);   /* blocking reap of the kill */
    }
    cs->pid = -1;
}

int camera_spawn_respawn(CameraSpawn *cs) {
    if (!cs) return -1;
    if (cs->read_fd >= 0) { close(cs->read_fd); cs->read_fd = -1; }

    /* Reap the OLD child before spawn_once overwrites cs->pid. We arrive here on
     * pipe EOF, where camera_spawn_reaped() (WNOHANG, and main.c DISCARDS its
     * result) may have returned 0 because the child closed stdout a hair before
     * becoming reapable -- leaving cs->pid still set. Without this, spawn_once
     * would orphan that exited child into a permanent zombie (one per cycle in
     * the rapid-death respawn loop). Bounded wait + SIGKILL escalation: the child
     * has already closed stdout, so it has exited or is about to. */
    reap_child_bounded(cs, 500);

    int b = cs->backoff_ms ? cs->backoff_ms : CAMERA_BACKOFF_MS_MIN;
    if (b > CAMERA_BACKOFF_MS_MAX) b = CAMERA_BACKOFF_MS_MAX;
    sleep_ms_interruptible(b);
    cs->backoff_ms = (b * 2 > CAMERA_BACKOFF_MS_MAX) ? CAMERA_BACKOFF_MS_MAX : b * 2;

    return spawn_once(cs);
}

void camera_spawn_terminate(CameraSpawn *cs) {
    if (!cs || cs->pid <= 0) return;
    if (kill(cs->pid, SIGTERM) != 0 && errno != ESRCH)
        fprintf(stderr, "pivid-tx: SIGTERM to rpicam-vid pid=%d failed: %s\n",
                (int)cs->pid, strerror(errno));
}

void camera_spawn_finish(CameraSpawn *cs, int deadline_ms) {
    if (!cs) return;

    /* Bounded reap (+ SIGKILL escalation) of the child. Usually returns on the
     * first probe -- teardown consumed its exit latency before we got here. */
    reap_child_bounded(cs, deadline_ms);

    /* Close the read-end LAST -- after the child is confirmed gone -- so a child
     * flushing final frames never takes SIGPIPE. Separate from tx_close(). */
    if (cs->read_fd >= 0) { close(cs->read_fd); cs->read_fd = -1; }
}
