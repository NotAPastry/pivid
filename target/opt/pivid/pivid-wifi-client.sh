#!/usr/bin/env bash
#
# pivid-wifi-client.sh get|set -- read/regenerate the WiFi *client* credentials
# (the SSID/password the Pi joins in normal device mode). Consumed by the Plan-4
# config web page. A change is applied when config mode exits (the orchestrator's
# down() runs `ifup wlan0`, re-reading this file) or on reboot.
#
#   get                : print  SSID=<ssid>  PSK_SET=0|1  COUNTRY=<cc>  parsed
#                        from wpa_supplicant.conf, for display. The stored psk is
#                        a one-way hash, so the password is never printed.
#   set <ssid> [<psk>] : regenerate wpa_supplicant.conf atomically (0600),
#                        preserving the country/ctrl_interface header. A non-empty
#                        psk is hashed via wpa_passphrase (WPA-PSK); an empty psk
#                        writes an open network (key_mgmt=NONE).
#
# Testability: WPA_CONF path + wpa_passphrase (PATH-resolved) are overridable.

set -uo pipefail

WPA_CONF="${PIVID_WPA_CONF:-/etc/wpa_supplicant/wpa_supplicant.conf}"

get() {
    local ssid="" psk_set=0 cc=""
    if [ -r "$WPA_CONF" ]; then
        ssid="$(grep -m1 -E '^[[:space:]]*ssid=' "$WPA_CONF" \
                | sed -E 's/^[[:space:]]*ssid="?([^"]*)"?.*/\1/')"
        grep -qE '^[[:space:]]*psk=' "$WPA_CONF" && psk_set=1
        cc="$(grep -m1 '^country=' "$WPA_CONF" | cut -d= -f2 | tr -cd 'A-Za-z')"
    fi
    printf 'SSID=%s\nPSK_SET=%s\nCOUNTRY=%s\n' "$ssid" "$psk_set" "$cc"
}

set_creds() {
    local ssid="${1:-}" psk="${2:-}"
    [ -n "$ssid" ] || { echo "pivid-wifi-client: empty SSID" >&2; return 2; }

    # Untrusted input (the open-AP web form feeds this, run as root):
    # reject anything that could break out of the quoted ssid="" field or inject
    # extra wpa_supplicant directives. SSID is 1..32 bytes of printable ASCII,
    # no double-quote or backslash. The first case arm catches control chars
    # (incl. newline) and non-ASCII; the second catches quote/backslash.
    case "$ssid" in
        *[!\ -~]*) echo "pivid-wifi-client: SSID has control or non-ASCII characters" >&2; return 2 ;;
        *\"*|*\\*) echo "pivid-wifi-client: SSID must not contain a quote or backslash" >&2; return 2 ;;
    esac
    [ "${#ssid}" -le 32 ] || { echo "pivid-wifi-client: SSID too long (max 32 bytes)" >&2; return 2; }

    # Passphrase (when set) is WPA-PSK: 8..63 chars, printable ASCII. (Only the
    # one-way hash is ever persisted, but validate for clean errors + defence.)
    if [ -n "$psk" ]; then
        case "$psk" in
            *[!\ -~]*) echo "pivid-wifi-client: passphrase has control or non-ASCII characters" >&2; return 2 ;;
        esac
        if [ "${#psk}" -lt 8 ] || [ "${#psk}" -gt 63 ]; then
            echo "pivid-wifi-client: passphrase must be 8..63 characters" >&2; return 2
        fi
    fi

    # Preserve the existing header; we write exactly one network block.
    local cc="" ctrl=""
    if [ -r "$WPA_CONF" ]; then
        cc="$(grep -m1 '^country=' "$WPA_CONF" || true)"
        ctrl="$(grep -m1 '^ctrl_interface=' "$WPA_CONF" || true)"
    fi
    [ -n "$ctrl" ] || ctrl="ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev"

    local dir tmp
    dir="$(dirname "$WPA_CONF")"
    tmp="$(mktemp "$dir/wpa.XXXXXX")" || return 1
    chmod 0600 "$tmp"
    {
        [ -n "$cc" ] && echo "$cc"
        echo "$ctrl"
        echo "update_config=1"
        if [ -n "$psk" ]; then
            # wpa_passphrase emits a network={...} with a HASHED psk plus a
            # commented #psk="plaintext" line -- strip the comment so the
            # plaintext secret is never persisted.
            wpa_passphrase "$ssid" "$psk" | grep -v '^[[:space:]]*#'
        else
            printf 'network={\n\tssid="%s"\n\tkey_mgmt=NONE\n}\n' "$ssid"
        fi
    } > "$tmp" || { rm -f "$tmp"; return 1; }
    sync
    mv -f "$tmp" "$WPA_CONF" || { rm -f "$tmp"; return 1; }
}

case "${1:-}" in
    get) get ;;
    set) shift; set_creds "$@" ;;
    *)   echo "usage: $0 get | set <ssid> [<psk>]" >&2; exit 2 ;;
esac
