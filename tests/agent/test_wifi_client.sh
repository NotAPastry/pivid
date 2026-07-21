#!/usr/bin/env bash
# Host test for pivid-wifi-client.sh. Uses fixture wpa_supplicant.conf files and
# a stub wpa_passphrase (so no real hashing/hardware). Verifies display parsing,
# atomic 0600 regeneration, PSK hashing, plaintext stripping, and open networks.
set -uo pipefail

SCRIPT="$(cd "$(dirname "$0")/../.." && pwd)/target/opt/pivid/pivid-wifi-client.sh"
fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  FAIL: $1" >&2; fail=1; fi; }

get_perms() { stat -c '%a' "$1" 2>/dev/null || stat -f '%OLp' "$1"; }

WORK="$(mktemp -d)"
# Stub wpa_passphrase: emit a block with a #plaintext comment + a hashed psk,
# exactly like the real tool, so the script's comment-stripping is exercised.
cat > "$WORK/wpa_passphrase" <<'EOF'
#!/bin/sh
printf 'network={\n\tssid="%s"\n\t#psk="%s"\n\tpsk=HASHED_%s\n}\n' "$1" "$2" "$2"
EOF
chmod +x "$WORK/wpa_passphrase"

echo "test_wifi_client: get parses a WPA-PSK config"
cat > "$WORK/source.conf" <<'EOF'
country=UA
ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev
update_config=1
network={
	ssid="ExampleNet"
	key_mgmt=WPA-PSK
	psk=e6b6236df77f
}
EOF
OUT="$(PIVID_WPA_CONF="$WORK/source.conf" "$SCRIPT" get)"
check "get SSID"      'echo "$OUT" | grep -q "^SSID=ExampleNet$"'
check "get PSK_SET=1" 'echo "$OUT" | grep -q "^PSK_SET=1$"'
check "get COUNTRY"   'echo "$OUT" | grep -q "^COUNTRY=UA$"'

echo "test_wifi_client: get reports an open network as PSK_SET=0"
cat > "$WORK/open.conf" <<'EOF'
country=UA
network={
	ssid="OpenNet"
	key_mgmt=NONE
}
EOF
OUT="$(PIVID_WPA_CONF="$WORK/open.conf" "$SCRIPT" get)"
check "get open SSID"      'echo "$OUT" | grep -q "^SSID=OpenNet$"'
check "get open PSK_SET=0" 'echo "$OUT" | grep -q "^PSK_SET=0$"'

echo "test_wifi_client: set with password -> hashed, 0600, header preserved"
cp "$WORK/source.conf" "$WORK/target.conf"
PATH="$WORK:$PATH" PIVID_WPA_CONF="$WORK/target.conf" "$SCRIPT" set "HomeWiFi" "s3cret-pass"; RC=$?
check "set exit ok"           '[ "$RC" -eq 0 ]'
check "set new ssid"          'grep -q "ssid=\"HomeWiFi\"" "$WORK/target.conf"'
check "set hashed psk"        'grep -q "psk=HASHED_s3cret-pass" "$WORK/target.conf"'
check "set strips plaintext"  '! grep -q "#psk=" "$WORK/target.conf"'
check "set preserves country" 'grep -q "^country=UA$" "$WORK/target.conf"'
check "set perms 0600"        '[ "$(get_perms "$WORK/target.conf")" = "600" ]'
check "set no temp left"      '! ls "$WORK"/wpa.* >/dev/null 2>&1'

echo "test_wifi_client: set without password -> open network"
PATH="$WORK:$PATH" PIVID_WPA_CONF="$WORK/open2.conf" "$SCRIPT" set "OpenAgain"; RC=$?
check "set open exit ok"   '[ "$RC" -eq 0 ]'
check "set open key_mgmt"   'grep -q "key_mgmt=NONE" "$WORK/open2.conf"'
check "set open no psk"     '! grep -qE "^[[:space:]]*psk=" "$WORK/open2.conf"'

echo "test_wifi_client: set with empty SSID is rejected"
PATH="$WORK:$PATH" PIVID_WPA_CONF="$WORK/never.conf" "$SCRIPT" set ""; RC=$?
check "set empty ssid fails" '[ "$RC" -ne 0 ]'
check "set empty wrote nothing" '! test -e "$WORK/never.conf"'

echo "test_wifi_client: SSID with a double-quote is rejected (no write)"
PATH="$WORK:$PATH" PIVID_WPA_CONF="$WORK/inj1.conf" "$SCRIPT" set 'evil"net' "goodpass1"; RC=$?
check "quote ssid rejected"      '[ "$RC" -ne 0 ]'
check "quote ssid wrote nothing" '! test -e "$WORK/inj1.conf"'

echo "test_wifi_client: SSID with an embedded newline is rejected (no write)"
PATH="$WORK:$PATH" PIVID_WPA_CONF="$WORK/inj2.conf" "$SCRIPT" set "$(printf 'a\nkey_mgmt=NONE')" "goodpass1"; RC=$?
check "newline ssid rejected"      '[ "$RC" -ne 0 ]'
check "newline ssid wrote nothing" '! test -e "$WORK/inj2.conf"'

echo "test_wifi_client: over-long SSID (>32 bytes) is rejected"
PATH="$WORK:$PATH" PIVID_WPA_CONF="$WORK/inj3.conf" "$SCRIPT" set "0123456789012345678901234567890123" "goodpass1"; RC=$?
check "long ssid rejected" '[ "$RC" -ne 0 ]'

echo "test_wifi_client: too-short passphrase rejected, original file untouched"
cp "$WORK/source.conf" "$WORK/shortpw.conf"
PATH="$WORK:$PATH" PIVID_WPA_CONF="$WORK/shortpw.conf" "$SCRIPT" set "HomeWiFi" "short"; RC=$?
check "short psk rejected"       '[ "$RC" -ne 0 ]'
check "short psk original intact" 'grep -q "ExampleNet" "$WORK/shortpw.conf"'
check "short psk no temp left"    '! ls "$WORK"/wpa.* >/dev/null 2>&1'

echo "test_wifi_client: wpa_passphrase failure leaves original untouched (atomic)"
cp "$WORK/source.conf" "$WORK/wpfail.conf"
FAILPP="$(mktemp -d)"; cat > "$FAILPP/wpa_passphrase" <<'PPEOF'
#!/bin/sh
exit 1
PPEOF
chmod +x "$FAILPP/wpa_passphrase"
PATH="$FAILPP:$PATH" PIVID_WPA_CONF="$WORK/wpfail.conf" "$SCRIPT" set "HomeWiFi" "validlen8"; RC=$?
check "wpa_passphrase fail -> nonzero" '[ "$RC" -ne 0 ]'
check "wpa_passphrase fail original intact" 'grep -q "ExampleNet" "$WORK/wpfail.conf"'
check "wpa_passphrase fail no temp left"    '! ls "$WORK"/wpa.* >/dev/null 2>&1'
rm -rf "$FAILPP"

rm -rf "$WORK"
if [ "$fail" -eq 0 ]; then echo "All wifi_client tests passed!"; else echo "wifi_client tests FAILED" >&2; fi
exit "$fail"
