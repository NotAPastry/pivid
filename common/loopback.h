/* common/loopback.h -- fixed rx -> caster handoff ports.
 *
 * pivid-rx and pivid-caster are always co-located on the RX Pi, so these are
 * compile-time constants, not config. The PLAYER's receive ports (5000/5001)
 * are separate and chosen player-side; these are only the internal loopback
 * hop. */
#ifndef COMMON_LOOPBACK_H
#define COMMON_LOOPBACK_H

#define LOOPBACK_ADDR        "127.0.0.1"
#define LOOPBACK_VIDEO_PORT  5600   /* raw Annex-B NAL datagrams      */
#define LOOPBACK_TELEM_PORT  5601   /* serialized OSD telemetry frames */

#endif
