/* Manual smoke check: the caster forwards a video datagram to a subscriber
 * after SUBSCRIBE, on loopback. Run on host. NOT part of `make run`
 * (sockets/threads/timing; binds the fixed caster ports). */
#define _POSIX_C_SOURCE 200809L
#include "caster.h"
#include "control.h"
#include "loopback.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_stop;

static int64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *run_caster(void *arg) {
    CasterCtx *c = arg;
    while (!atomic_load(&g_stop))
        caster_step(c, 50, mono_ms());
    return NULL;
}

static int connect_to(uint16_t port) {   /* a sender to a loopback port */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    assert(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    return fd;
}

int main(void) {
    /* The player's video receive socket on a free port. */
    const uint16_t PLAYER_VPORT = 5000, PLAYER_TPORT = 5001;
    int pfd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(pfd >= 0);
    struct sockaddr_in pa; memset(&pa, 0, sizeof pa);
    pa.sin_family = AF_INET; pa.sin_port = htons(PLAYER_VPORT);
    inet_pton(AF_INET, "127.0.0.1", &pa.sin_addr);
    assert(bind(pfd, (struct sockaddr *)&pa, sizeof pa) == 0);

    CasterCtx caster;
    assert(caster_open(&caster, false) == 0);   /* forwarding OFF: loopback still served */

    pthread_t th;
    atomic_store(&g_stop, 0);
    assert(pthread_create(&th, NULL, run_caster, &caster) == 0);

    /* SUBSCRIBE from loopback (accepted even with forwarding OFF). */
    int cfd = connect_to((uint16_t)CASTER_CTRL_PORT);
    uint8_t sub[CASTER_CTRL_SUBSCRIBE_LEN];
    caster_ctrl_build_subscribe(sub, sizeof sub, PLAYER_VPORT, PLAYER_TPORT);
    assert(send(cfd, sub, sizeof sub, 0) == (ssize_t)sizeof sub);

    /* Give the caster a moment to process the SUBSCRIBE. */
    struct timespec d = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000L };
    nanosleep(&d, NULL);

    /* Send a video datagram into the caster's video-in port. */
    int vfd = connect_to((uint16_t)LOOPBACK_VIDEO_PORT);
    const uint8_t nal[] = { 0x00,0x00,0x00,0x01, 0x67, 0xDE, 0xAD, 0xBE, 0xEF };
    assert(send(vfd, nal, sizeof nal, 0) == (ssize_t)sizeof nal);

    /* The player must receive the forwarded datagram verbatim. */
    uint8_t got[64];
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(pfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ssize_t n = recv(pfd, got, sizeof got, 0);
    assert(n == (ssize_t)sizeof nal);
    assert(memcmp(got, nal, sizeof nal) == 0);

    atomic_store(&g_stop, 1);
    pthread_join(th, NULL);
    caster_close(&caster);
    close(pfd); close(cfd); close(vfd);
    printf("test_caster_smoke PASS (forwarded %zd bytes verbatim)\n", n);
    return 0;
}
