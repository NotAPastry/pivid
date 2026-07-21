#!/usr/bin/env bash
# Structural lint of the Plan-3 systemd units. Greps required directives so a
# broken dependency graph (e.g. missing PartOf -> no teardown) fails in CI, not
# on the bench.
set -uo pipefail

D="$(cd "$(dirname "$0")/../.." && pwd)/target/etc/systemd/system"
fail=0
has() { # has <file> <grep-ere> <label>
    if grep -Eq "$2" "$D/$1"; then echo "  ok: $3"; else echo "  FAIL: $3" >&2; fail=1; fi
}

echo "test_config_units: pivid-config-agent.service"
has pivid-config-agent.service '^ExecStart=/opt/pivid/pivid-config-agent' "agent ExecStart"
has pivid-config-agent.service '^WantedBy=multi-user\.target'             "agent enabled at boot"

echo "test_config_units: pivid-config-net.service"
has pivid-config-net.service  '^Type=oneshot'                          "net Type=oneshot"
has pivid-config-net.service  '^RemainAfterExit=yes'                   "net RemainAfterExit"
# KillMode=process so the client wpa_supplicant/dhclient that down() spawns
# survive the unit's stop (default control-group would reap them).
has pivid-config-net.service  '^KillMode=process'                     "net KillMode=process"
has pivid-config-net.service  '^ExecStart=/opt/pivid/pivid-config-net\.sh up'    "net ExecStart up"
has pivid-config-net.service  '^ExecStop=/opt/pivid/pivid-config-net\.sh down'   "net ExecStop down"
has pivid-config-net.service  '^ExecStartPost=.*touch /run/pivid/config-mode'    "net touches flag"
has pivid-config-net.service  '^ExecStopPost=.*rm -f /run/pivid/config-mode'     "net removes flag"

echo "test_config_units: hostapd + dnsmasq helper units"
for u in pivid-config-hostapd pivid-config-dnsmasq; do
    has "$u.service" '^Type=forking'                  "$u Type=forking"
    has "$u.service" '^PartOf=pivid-config-net\.service'   "$u PartOf net"
    # MUST NOT have After=net: the net oneshot starts these from its ExecStart,
    # so an After=net ordering deadlocks net's activation (hardware-confirmed).
    if grep -Eq '^After=pivid-config-net\.service' "$D/$u.service"; then
        echo "  FAIL: $u must NOT have After=pivid-config-net.service (deadlock)" >&2; fail=1
    else echo "  ok: $u has no After=net (deadlock-safe)"; fi
    has "$u.service" '^PIDFile=/run/pivid/'              "$u PIDFile set"
    has "$u.service" '^RestartPreventExitStatus=SIGTERM' "$u suppresses clean-stop restart"
done
has pivid-config-hostapd.service '^ExecStart=/usr/sbin/hostapd' "hostapd ExecStart"
has pivid-config-dnsmasq.service '^ExecStart=/usr/sbin/dnsmasq' "dnsmasq ExecStart"

echo "test_config_units: local HDMI player path is WiFi-independent"
# The rx -> caster -> player video path is loopback only, but network.target on
# this DietPi image is gated by ifup@wlan0.service (Before=network.target,
# wpa_supplicant + dhclient run synchronously, TimeoutStartSec=5min). An
# After=network.target anywhere in this chain therefore holds the HDMI picture
# hostage to WiFi association: measured 60.2s with WiFi UP, ~5min with no AP in
# range. Neither unit needs an interface to exist at start (loopback/INADDR_ANY
# binds, unbound fan-out socket, AVAHI_IF_UNSPEC mDNS) -- see the long note in
# pivid-caster.service. Assert no one re-adds it.
for u in pivid-caster pivid-player; do
    if grep -Eq '^After=.*\bnetwork(-online)?\.target\b' "$D/$u.service"; then
        echo "  FAIL: $u must NOT be ordered after network.target (blocks HDMI on WiFi)" >&2; fail=1
    else echo "  ok: $u has no After=network.target (WiFi-independent)"; fi
done
has pivid-player.service '^After=pivid-caster\.service'  "player ordered after caster"
has pivid-player.service '^Wants=pivid-caster\.service'  "player Wants (not Requires) caster"

if [ "$fail" -eq 0 ]; then echo "All config_units tests passed!"; else echo "config_units tests FAILED" >&2; fi
exit "$fail"
