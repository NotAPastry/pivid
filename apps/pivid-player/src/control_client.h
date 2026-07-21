/* apps/pivid-player/src/control_client.h -- the SUBSCRIBE client. Sends
 * SUBSCRIBE{video_port, telem_port} to caster_ip:control_port, re-sends every
 * ~2s as the keepalive (the caster ages out at 5s), and sends one UNSUBSCRIBE on
 * stop. Uses common/control.h builders. */
#ifndef PLAYER_CONTROL_CLIENT_H
#define PLAYER_CONTROL_CLIENT_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    char         caster_ip[64];
    uint16_t     control_port;
    uint16_t     video_port;
    uint16_t     telem_port;
    int          fd;
    atomic_bool  stop;
    pthread_t    thread;
} ControlClient;

int  control_client_start(ControlClient *c, const char *caster_ip,
                          uint16_t control_port, uint16_t video_port,
                          uint16_t telem_port);
void control_client_stop(ControlClient *c);   /* sends UNSUBSCRIBE, then joins */

#endif
