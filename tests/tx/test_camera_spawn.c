/* Linux integration tests for camera_spawn: spawn + read + reap, exec failure,
 * and respawn. Uses a stub "camera" binary (a tiny shell script) pointed at via
 * PIVID_RPICAM_BIN, so no real rpicam-vid/libcamera is needed. Runs in the
 * gcc:bookworm x86_64 test container. Terminate/finish cases follow below. */
#define _GNU_SOURCE
#include "camera_spawn.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", (msg)); fails++; } \
                           else { printf("OK: %s\n", (msg)); } } while (0)

/* Write an executable stub script to `path` with the given body, set
 * PIVID_RPICAM_BIN to it. The body ignores its rpicam-style args ("$@"). */
static void make_stub(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("#!/bin/sh\n", f);
    fputs(body, f);
    fputs("\n", f);
    fclose(f);
    assert(chmod(path, 0755) == 0);
    setenv("PIVID_RPICAM_BIN", path, 1);
}

static Settings make_settings(void) {
    Settings s; memset(&s, 0, sizeof s);
    snprintf(s.recipe, sizeof s.recipe, "%s", "droppable");
    s.width = 640; s.height = 480; s.fps = 30; s.bitrate = 650000;
    s.intra = 30; s.slice_mb = 120; s.droppable_len = 8; s.idr_period = 60;
    return s;
}

/* Reap with a bounded retry. Pipe EOF (write-end closed) can be observed before
 * the kernel has transitioned the child to a reapable zombie, so a single
 * waitpid(WNOHANG) probe races: camera_spawn_reaped() correctly returns 0 ("not
 * reaped yet") in that window. Tests want determinism, so retry briefly until the
 * child is collected. Returns the final camera_spawn_reaped() result. */
static int reap_settled(CameraSpawn *cs) {
    int r = 0;
    for (int i = 0; i < 200; i++) {           /* up to ~1s */
        r = camera_spawn_reaped(cs);
        if (r != 0) break;                    /* 1 = reaped, -1 = error: stop */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5L * 1000000L };
        nanosleep(&ts, NULL);
    }
    return r;
}

/* Read up to cap bytes from fd, blocking briefly via poll; returns count. */
static size_t drain(int fd, char *buf, size_t cap) {
    size_t got = 0;
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, 2000);
        if (pr <= 0) break;
        ssize_t n = read(fd, buf + got, cap - got);
        if (n <= 0) break;            /* 0 = EOF */
        got += (size_t)n;
        if (got >= cap) break;
    }
    return got;
}

static void test_spawn_read_reap(void) {
    make_stub("/tmp/pivid_stub_ok.sh", "printf 'HELLOSTREAM'; exit 0");
    Settings s = make_settings();
    CameraSpawn cs;
    CHECK(camera_spawn_start(&cs, &s) == 0, "start ok-stub returns 0");
    CHECK(cs.pid > 0, "child pid set");
    CHECK(cs.read_fd >= 0, "read_fd set");
    CHECK((fcntl(cs.read_fd, F_GETFD) & FD_CLOEXEC) != 0, "read_fd is O_CLOEXEC");

    char buf[64] = {0};
    size_t n = drain(cs.read_fd, buf, sizeof buf);
    CHECK(n == 11 && memcmp(buf, "HELLOSTREAM", 11) == 0, "received stub bytes then EOF");

    int r = reap_settled(&cs);
    CHECK(r == 1, "reaped returns 1 (child gone)");
    CHECK(cs.pid == -1, "pid cleared after reap");
    close(cs.read_fd);
}

static void test_exec_failure(void) {
    setenv("PIVID_RPICAM_BIN", "/nonexistent/rpicam-vid", 1);
    Settings s = make_settings();
    CameraSpawn cs;
    /* start() still returns 0 (fork succeeded); the child exec-fails and exits. */
    CHECK(camera_spawn_start(&cs, &s) == 0, "start returns 0 even when exec will fail");
    char buf[16];
    size_t n = drain(cs.read_fd, buf, sizeof buf);
    CHECK(n == 0, "exec failure -> immediate EOF (no bytes)");
    CHECK(reap_settled(&cs) == 1, "exec-fail child reaped");
    close(cs.read_fd);
}

static void test_bad_recipe(void) {
    setenv("PIVID_RPICAM_BIN", "/bin/true", 1);
    Settings s = make_settings();
    snprintf(s.recipe, sizeof s.recipe, "%s", "bogus");
    CameraSpawn cs;
    CHECK(camera_spawn_start(&cs, &s) == -1, "bad recipe -> start returns -1 (no fork)");
}

static void test_respawn(void) {
    make_stub("/tmp/pivid_stub_ok.sh", "printf 'X'; exit 0");
    Settings s = make_settings();
    CameraSpawn cs;
    assert(camera_spawn_start(&cs, &s) == 0);
    int old_fd = cs.read_fd; (void)old_fd;
    char buf[8];
    (void)drain(cs.read_fd, buf, sizeof buf);
    assert(reap_settled(&cs) == 1);

    CHECK(camera_spawn_respawn(&cs) == 0, "respawn returns 0 (new child)");
    CHECK(cs.pid > 0, "respawn set a new pid");
    CHECK(cs.read_fd >= 0, "respawn opened a new read_fd");
    CHECK(cs.backoff_ms >= CAMERA_BACKOFF_MS_MIN, "backoff advanced");
    (void)drain(cs.read_fd, buf, sizeof buf);
    (void)reap_settled(&cs);
    close(cs.read_fd);
}

static void test_respawn_reaps_old_child(void) {
    /* Regression: main.c discards camera_spawn_reaped()'s result, and on pipe
     * EOF that WNOHANG probe can return 0 (child closed stdout a hair before
     * becoming reapable), leaving cs->pid set. respawn() MUST reap that old child
     * before spawn_once overwrites cs->pid -- else it leaks a permanent zombie.
     * This test reproduces the discarded-reaped() path: drain to EOF, then call
     * respawn WITHOUT reaping first, and assert the old child is no longer
     * waitable (ECHILD = reaped, not a lingering zombie). */
    make_stub("/tmp/pivid_stub_ok.sh", "printf 'X'; exit 0");
    Settings s = make_settings();
    CameraSpawn cs;
    assert(camera_spawn_start(&cs, &s) == 0);
    pid_t old_pid = cs.pid;
    char buf[8];
    (void)drain(cs.read_fd, buf, sizeof buf);   /* read to EOF; do NOT reap */

    CHECK(camera_spawn_respawn(&cs) == 0, "respawn (no prior reap) returns 0");
    int st;
    CHECK(waitpid(old_pid, &st, WNOHANG) == -1 && errno == ECHILD,
          "respawn reaped the old child (no zombie)");
    CHECK(cs.pid > 0 && cs.pid != old_pid, "respawn set a fresh pid");

    (void)drain(cs.read_fd, buf, sizeof buf);
    (void)reap_settled(&cs);
    close(cs.read_fd);
}

static void test_terminate_graceful(void) {
    /* Stub installs a SIGTERM handler that exits 0 quickly. */
    make_stub("/tmp/pivid_stub_term.sh",
              "trap 'exit 0' TERM; while :; do sleep 0.05; done");
    Settings s = make_settings();
    CameraSpawn cs;
    assert(camera_spawn_start(&cs, &s) == 0);
    pid_t child = cs.pid;

    camera_spawn_terminate(&cs);            /* SIGTERM */
    camera_spawn_finish(&cs, 2000);         /* bounded wait reaps it */
    CHECK(cs.pid == -1, "graceful: child reaped");
    CHECK(cs.read_fd == -1, "graceful: read_fd closed last");
    CHECK(kill(child, 0) == -1 && errno == ESRCH, "graceful: child truly gone");
}

static void test_terminate_escalates_sigkill(void) {
    /* Stub IGNORES SIGTERM and sleeps -> finish must escalate to SIGKILL. */
    make_stub("/tmp/pivid_stub_ignore.sh",
              "trap '' TERM; while :; do sleep 0.05; done");
    Settings s = make_settings();
    CameraSpawn cs;
    assert(camera_spawn_start(&cs, &s) == 0);
    pid_t child = cs.pid;

    /* Let the stub exec /bin/sh and install `trap '' TERM` before we SIGTERM it,
     * so SIGTERM is genuinely ignored and finish() must escalate to SIGKILL.
     * Without this settle the SIGTERM races ahead of the trap (disposition still
     * SIG_DFL post-exec) and the child dies on TERM -- a false-green that never
     * exercises the SIGKILL path. */
    struct timespec settle = { .tv_sec = 0, .tv_nsec = 200L * 1000000L };
    nanosleep(&settle, NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    camera_spawn_terminate(&cs);
    camera_spawn_finish(&cs, 300);          /* short deadline -> SIGKILL path */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long finish_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
                     (t1.tv_nsec - t0.tv_nsec) / 1000000;

    CHECK(cs.pid == -1, "escalation: child reaped after SIGKILL");
    CHECK(kill(child, 0) == -1 && errno == ESRCH, "escalation: child gone");
    CHECK(finish_ms >= 250, "escalation: finish waited the deadline then SIGKILL");
}

static void test_finish_idempotent(void) {
    CameraSpawn cs; memset(&cs, 0, sizeof cs);
    cs.pid = -1; cs.read_fd = -1;
    camera_spawn_terminate(&cs);            /* no-op, no crash */
    camera_spawn_finish(&cs, 100);          /* no-op, no crash */
    CHECK(1, "finish/terminate safe when pid already -1");
}

int main(void) {
    test_spawn_read_reap();
    test_exec_failure();
    test_bad_recipe();
    test_respawn();
    test_respawn_reaps_old_child();
    test_terminate_graceful();
    test_terminate_escalates_sigkill();
    test_finish_idempotent();
    if (fails) { printf("%d FAILURE(S)\n", fails); return 1; }
    printf("all camera_spawn tests passed\n");
    return 0;
}
