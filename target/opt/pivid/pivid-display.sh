#!/usr/bin/env bash
#
# pivid-display.sh set {hdmi | composite [pal|ntsc]}
#
# Manage the RX local display output by maintaining a marked, idempotent block in
# the boot firmware config.txt. Requires a REBOOT to take effect.
#   composite -> enable_tvout=1 + sdtv_mode (PAL=2, NTSC=0), HDMI off (firmware).
#   hdmi      -> remove the block (HDMI is the firmware default).
#
# Testability: PIVID_CONFIG_TXT overrides the target file.

set -uo pipefail

CONFIG_TXT="${PIVID_CONFIG_TXT:-}"
if [ -z "$CONFIG_TXT" ]; then
    if   [ -f /boot/firmware/config.txt ]; then CONFIG_TXT=/boot/firmware/config.txt
    elif [ -f /boot/config.txt ];          then CONFIG_TXT=/boot/config.txt
    else echo "pivid-display: no config.txt found" >&2; exit 1; fi
fi

BEGIN='# pivid-composite BEGIN (managed by pivid-display.sh -- do not edit)'
END='# pivid-composite END'

# Print CONFIG_TXT with any existing managed block removed.
strip_block() { sed '/^# pivid-composite BEGIN/,/^# pivid-composite END/d' "$CONFIG_TXT"; }

# Read stdin, write it to CONFIG_TXT atomically (temp in same dir + rename).
write_atomic() {
    local dir tmp
    dir="$(dirname "$CONFIG_TXT")"
    tmp="$(mktemp "$dir/cfg.XXXXXX")" || return 1
    cat > "$tmp" || { rm -f "$tmp"; return 1; }
    sync
    mv -f "$tmp" "$CONFIG_TXT" || { rm -f "$tmp"; return 1; }
}

set_output() {
    local mode="${1:-}" std="${2:-pal}" sdtv
    case "$mode" in
        hdmi)
            # $(strip_block) drops ALL trailing newlines; printf re-adds exactly
            # one, so the file always ends in a single newline (no corruption if
            # the original lacked one).
            printf '%s\n' "$(strip_block)" | write_atomic ;;
        composite)
            case "$std" in
                pal)  sdtv=2 ;;
                ntsc) sdtv=0 ;;
                *) echo "pivid-display: standard must be pal|ntsc" >&2; return 2 ;;
            esac
            # Normalise the stripped content to end in exactly one newline (see
            # hdmi note) BEFORE appending, so the BEGIN marker always starts its
            # own line -- otherwise strip_block could never remove it again and
            # repeated applies would stack blocks.
            { printf '%s\n' "$(strip_block)"
              printf '%s\nenable_tvout=1\nsdtv_mode=%s\n%s\n' \
                  "$BEGIN" "$sdtv" "$END"; } | write_atomic ;;
        *) echo "pivid-display: mode must be hdmi|composite" >&2; return 2 ;;
    esac
}

case "${1:-}" in
    set) shift; set_output "$@" ;;
    *)   echo "usage: $0 set {hdmi|composite [pal|ntsc]}" >&2; exit 2 ;;
esac
