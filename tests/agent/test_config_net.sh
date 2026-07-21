#!/usr/bin/env bash
# Host test for pivid-config-net.sh. Stubs ifdown/ifup/ip/systemctl/pkill/iw/
# hostname/hostapd_cli onto PATH (each logs its argv to $CALLS and exits per
# $FAIL_UNIT / $HOSTAPD_ENABLE_AFTER), then asserts the orchestrator's behaviour
# with no real hardware.
set -uo pipefail

SCRIPT="$(cd "$(dirname "$0")/../.." && pwd)/target/opt/pivid/pivid-config-net.sh"
fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  FAIL: $1" >&2; fail=1; fi; }

# Build a throwaway stub dir. Each stub appends "<name> <args>" to $CALLS and
# honours $FAIL_UNIT: if `systemctl start <FAIL_UNIT>` is called, that stub
# returns non-zero (to exercise rollback). hostapd_cli reports state=ENABLED once
# hostapd has been started $HOSTAPD_ENABLE_AFTER times (default 1 -> first try).
make_env() {
    STUBDIR="$(mktemp -d)"
    CALLS="$STUBDIR/calls.log"
    : > "$CALLS"
    for cmd in ifdown ifup ip pkill iw; do
        cat > "$STUBDIR/$cmd" <<EOF
#!/bin/sh
echo "$cmd \$*" >> "$CALLS"
exit 0
EOF
        chmod +x "$STUBDIR/$cmd"
    done
    cat > "$STUBDIR/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >> "$CALLS"
if [ "\$1" = "start" ] && [ -n "\${FAIL_UNIT:-}" ] && [ "\$2" = "\$FAIL_UNIT" ]; then
    exit 1
fi
exit 0
EOF
    chmod +x "$STUBDIR/systemctl"
    cat > "$STUBDIR/hostapd_cli" <<EOF
#!/bin/sh
echo "hostapd_cli \$*" >> "$CALLS"
starts=\$(grep -c "systemctl start pivid-config-hostapd.service" "$CALLS" 2>/dev/null)
[ -z "\$starts" ] && starts=0
if [ "\$starts" -ge "\${HOSTAPD_ENABLE_AFTER:-1}" ]; then
    echo "state=ENABLED"
else
    echo "state=COUNTRY_UPDATE"
fi
EOF
    chmod +x "$STUBDIR/hostapd_cli"
    cat > "$STUBDIR/hostname" <<EOF
#!/bin/sh
echo "\${PIVID_HOSTNAME:-pivid-tx}"
EOF
    chmod +x "$STUBDIR/hostname"
    # Fixture wpa_supplicant.conf (its country= must NOT leak into hostapd.conf).
    printf 'country=UA\nnetwork={\n\tssid="ExampleNet"\n}\n' > "$STUBDIR/wpa.conf"
}

run() {  # run <up|down>; sets RC and exposes $CALLS / generated conf. Fast waits.
    PATH="$STUBDIR:$PATH" \
    PIVID_RUN_DIR="$STUBDIR/run" \
    PIVID_WPA_CONF="$STUBDIR/wpa.conf" \
    PIVID_AP_WAIT_SLEEP=0 \
    PIVID_AP_WAIT_TRIES="${TEST_WAIT_TRIES:-3}" \
    PIVID_AP_RETRIES="${TEST_AP_RETRIES:-3}" \
    "$SCRIPT" "$1"
    RC=$?
}

echo "test_config_net: up (TX) fences client, brings up AP, verifies AP-ENABLED"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
run up
check "up exits 0"                 '[ "$RC" -eq 0 ]'
check "boot client stopped"        'grep -q "systemctl stop ifup@wlan0.service" "$CALLS"'
check "boot client runtime-masked" 'grep -q "systemctl mask --runtime ifup@wlan0.service" "$CALLS"'
check "static IP assigned"         'grep -q "ip addr add 10.0.0.1/24 dev wlan0" "$CALLS"'
check "hostapd unit started"       'grep -q "systemctl start pivid-config-hostapd.service" "$CALLS"'
check "dnsmasq unit started"       'grep -q "systemctl start pivid-config-dnsmasq.service" "$CALLS"'
check "AP-ENABLED verified"        'grep -q "hostapd_cli .*status" "$CALLS"'
check "TX SSID generated"          'grep -q "ssid=PiVid-TX" "$STUBDIR/run/hostapd.conf"'
check "ctrl_interface present"     'grep -q "^ctrl_interface=" "$STUBDIR/run/hostapd.conf"'
check "no country_code (802.11d dropped)" '! grep -q "country_code=" "$STUBDIR/run/hostapd.conf"'
check "no ieee80211d (802.11d dropped)"   '! grep -q "ieee80211d" "$STUBDIR/run/hostapd.conf"'
check "success leaves client masked (no unmask)" '! grep -q "systemctl unmask" "$CALLS"'
# dnsmasq.conf is GENERATED, not shipped verbatim: the interface and every
# address must be derived from the same variables hostapd and `ip addr` use, or
# the captive portal binds an interface nobody is on. No placeholder may survive.
check "dnsmasq conf generated"     '[ -r "$STUBDIR/run/dnsmasq.conf" ]'
check "dnsmasq binds WLAN_IF"      'grep -q "^interface=wlan0$" "$STUBDIR/run/dnsmasq.conf"'
check "dnsmasq router = AP_ADDR"   'grep -q "^dhcp-option=option:router,10.0.0.1$" "$STUBDIR/run/dnsmasq.conf"'
check "dnsmasq pool in AP subnet"  'grep -q "^dhcp-range=10.0.0.10,10.0.0.100," "$STUBDIR/run/dnsmasq.conf"'
check "dnsmasq wildcard = AP_ADDR" 'grep -q "^address=/#/10.0.0.1$" "$STUBDIR/run/dnsmasq.conf"'
check "no unsubstituted placeholders" '! grep -q "@" "$STUBDIR/run/dnsmasq.conf"'
rm -rf "$STUBDIR"

echo "test_config_net: dnsmasq conf tracks a non-default interface + AP subnet"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
PIVID_WLAN_IF=wlan1 PIVID_AP_ADDR=10.9.9.1 run up
check "dnsmasq follows PIVID_WLAN_IF"  'grep -q "^interface=wlan1$" "$STUBDIR/run/dnsmasq.conf"'
check "dnsmasq follows PIVID_AP_ADDR"  'grep -q "^address=/#/10.9.9.1$" "$STUBDIR/run/dnsmasq.conf"'
check "dnsmasq pool follows AP subnet" 'grep -q "^dhcp-range=10.9.9.10,10.9.9.100," "$STUBDIR/run/dnsmasq.conf"'
check "hostapd agrees on interface"    'grep -q "^interface=wlan1$" "$STUBDIR/run/hostapd.conf"'
rm -rf "$STUBDIR"

echo "test_config_net: up (RX) derives PiVid-RX SSID"
make_env; PIVID_HOSTNAME=pivid-rx; export PIVID_HOSTNAME
run up
check "RX SSID generated"          'grep -q "ssid=PiVid-RX" "$STUBDIR/run/hostapd.conf"'
rm -rf "$STUBDIR"

echo "test_config_net: up retries until hostapd reaches AP-ENABLED"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
HOSTAPD_ENABLE_AFTER=2; export HOSTAPD_ENABLE_AFTER
run up
unset HOSTAPD_ENABLE_AFTER
check "up eventually succeeds"      '[ "$RC" -eq 0 ]'
check "hostapd started >=2 times"  '[ "$(grep -c "systemctl start pivid-config-hostapd.service" "$CALLS")" -ge 2 ]'
rm -rf "$STUBDIR"

echo "test_config_net: up rolls back when hostapd never enables"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
HOSTAPD_ENABLE_AFTER=99; export HOSTAPD_ENABLE_AFTER
run up
unset HOSTAPD_ENABLE_AFTER
check "up reports failure (never enabled)" '[ "$RC" -ne 0 ]'
check "exhausted all 3 attempts"           '[ "$(grep -c "systemctl start pivid-config-hostapd.service" "$CALLS")" -eq 3 ]'
check "rollback unmasked client"           'grep -q "systemctl unmask.*ifup@wlan0.service" "$CALLS"'
rm -rf "$STUBDIR"

echo "test_config_net: down tears down AP, unmasks + restores client, clears flag"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
mkdir -p "$STUBDIR/run"; : > "$STUBDIR/run/config-mode"   # pre-existing flag
PIVID_FLAG="$STUBDIR/run/config-mode" run down
check "down exits 0"               '[ "$RC" -eq 0 ]'
check "dnsmasq unit stopped"       'grep -q "systemctl stop pivid-config-dnsmasq.service" "$CALLS"'
check "hostapd unit stopped"       'grep -q "systemctl stop pivid-config-hostapd.service" "$CALLS"'
check "client unmasked"            'grep -q "systemctl unmask.*ifup@wlan0.service" "$CALLS"'
check "client restored via unit"   'grep -q "systemctl start ifup@wlan0.service" "$CALLS"'
check "managed-mode reset"         'grep -q "iw dev wlan0 set type managed" "$CALLS"'
check "flag cleared by down"       '! test -e "$STUBDIR/run/config-mode"'
rm -rf "$STUBDIR"

echo "test_config_net: hostapd failure rolls back to client mode"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
FAIL_UNIT=pivid-config-hostapd.service; export FAIL_UNIT
run up
unset FAIL_UNIT
check "up reports failure"         '[ "$RC" -ne 0 ]'
check "rollback unmasked client"   'grep -q "systemctl unmask.*ifup@wlan0.service" "$CALLS"'
rm -rf "$STUBDIR"

echo "test_config_net: dnsmasq failure rolls back to client mode"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
FAIL_UNIT=pivid-config-dnsmasq.service; export FAIL_UNIT
run up
unset FAIL_UNIT
check "up reports failure (dnsmasq)" '[ "$RC" -ne 0 ]'
check "rollback unmasked (dnsmasq)"  'grep -q "systemctl unmask.*ifup@wlan0.service" "$CALLS"'
rm -rf "$STUBDIR"

echo "test_config_net: missing wpa.conf still produces a valid open AP conf"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
rm -f "$STUBDIR/wpa.conf"
run up
check "no country_code when wpa.conf absent" '! grep -q "country_code=" "$STUBDIR/run/hostapd.conf"'
check "SSID still generated"                 'grep -q "ssid=PiVid-TX" "$STUBDIR/run/hostapd.conf"'
rm -rf "$STUBDIR"

echo "test_config_net: unknown arg exits 2"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
PATH="$STUBDIR:$PATH" PIVID_RUN_DIR="$STUBDIR/run" PIVID_WPA_CONF="$STUBDIR/wpa.conf" "$SCRIPT" bogus; RC=$?
check "unknown arg exits 2" '[ "$RC" -eq 2 ]'
rm -rf "$STUBDIR"

echo "test_config_net: WPA2 from PIVID_AP_PASSWORD=broadcast (>=8 chars)"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
PIVID_AP_PASSWORD="broadcast"; export PIVID_AP_PASSWORD
run up
unset PIVID_AP_PASSWORD
check "wpa=2 present"                'grep -q "^wpa=2$"                    "$STUBDIR/run/hostapd.conf"'
check "wpa_key_mgmt present"         'grep -q "^wpa_key_mgmt=WPA-PSK$"     "$STUBDIR/run/hostapd.conf"'
check "wpa_passphrase present"       'grep -q "^wpa_passphrase=broadcast$" "$STUBDIR/run/hostapd.conf"'
rm -rf "$STUBDIR"

echo "test_config_net: WPA2 blank password -> open AP (no wpa lines)"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
PIVID_AP_PASSWORD=""; export PIVID_AP_PASSWORD
run up
unset PIVID_AP_PASSWORD
check "blank pw => open AP (no wpa= line)" '! grep -q "^wpa=" "$STUBDIR/run/hostapd.conf"'
rm -rf "$STUBDIR"

echo "test_config_net: WPA2 too-short password (<8) -> open AP (rejected)"
make_env; PIVID_HOSTNAME=pivid-tx; export PIVID_HOSTNAME
PIVID_AP_PASSWORD="short"; export PIVID_AP_PASSWORD
run up
unset PIVID_AP_PASSWORD
check "short pw => open AP (no wpa= line)" '! grep -q "^wpa=" "$STUBDIR/run/hostapd.conf"'
rm -rf "$STUBDIR"

echo "wpa2 hostapd generation: OK (markers below)"

if [ "$fail" -eq 0 ]; then echo "All config_net tests passed!"; else echo "config_net tests FAILED" >&2; fi
exit "$fail"
