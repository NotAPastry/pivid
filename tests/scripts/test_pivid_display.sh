#!/usr/bin/env bash
# Standalone test for pivid-display.sh: run `sh tests/scripts/test_pivid_display.sh`.
# NOTE: the helper is bash (shebang + `set -o pipefail`), so exec it DIRECTLY via
# its shebang (as production does) -- never `sh "$HELPER"`, which would route it
# through dash on the target and choke on pipefail.
set -u
HELPER="$(dirname "$0")/../../target/opt/pivid/pivid-display.sh"
fail=0
chk() { if eval "$2"; then echo "ok: $1"; else echo "FAIL: $1"; fail=1; fi; }

TMP="$(mktemp -d)"; CFG="$TMP/config.txt"
printf 'dtoverlay=vc4-kms-v3d\n# existing\n' > "$CFG"
export PIVID_CONFIG_TXT="$CFG"

# composite PAL -> block with enable_tvout + sdtv_mode=2
"$HELPER" set composite pal
chk "PAL enable_tvout"   "grep -q '^enable_tvout=1' '$CFG'"
chk "PAL sdtv_mode=2"    "grep -q '^sdtv_mode=2' '$CFG'"
chk "PAL keeps existing" "grep -q '^dtoverlay=vc4-kms-v3d' '$CFG'"

# composite NTSC -> sdtv_mode=0, single block (idempotent replace)
"$HELPER" set composite ntsc
chk "NTSC sdtv_mode=0"   "grep -q '^sdtv_mode=0' '$CFG'"
chk "no PAL leftover"    "! grep -q '^sdtv_mode=2' '$CFG'"
chk "one BEGIN marker"   "[ \"\$(grep -c 'pivid-composite BEGIN' '$CFG')\" = 1 ]"

# hdmi -> block removed entirely
"$HELPER" set hdmi
chk "hdmi removes block" "! grep -q 'pivid-composite' '$CFG'"
chk "hdmi no tvout"      "! grep -q 'enable_tvout' '$CFG'"
chk "hdmi keeps existing" "grep -q '^dtoverlay=vc4-kms-v3d' '$CFG'"

# Regression: config.txt with NO trailing newline must not glue the BEGIN marker
# onto the last existing line (which would corrupt it AND make the block
# un-strippable -> stacking + hdmi-can't-remove).
CFG2="$TMP/config2.txt"
printf 'dtoverlay=vc4-kms-v3d' > "$CFG2"   # note: NO trailing newline
export PIVID_CONFIG_TXT="$CFG2"
"$HELPER" set composite pal
chk "nonl dtoverlay intact"  "grep -q '^dtoverlay=vc4-kms-v3d\$' '$CFG2'"
chk "nonl BEGIN own line"    "grep -q '^# pivid-composite BEGIN' '$CFG2'"
# Idempotency under this condition: apply twice more -> still exactly one block.
"$HELPER" set composite pal
"$HELPER" set composite pal
chk "nonl one BEGIN marker"  "[ \"\$(grep -c 'pivid-composite BEGIN' '$CFG2')\" = 1 ]"
# hdmi must fully remove the block and leave the dtoverlay line intact.
"$HELPER" set hdmi
chk "nonl hdmi removes block" "! grep -q 'pivid-composite' '$CFG2'"
chk "nonl hdmi keeps line"    "grep -q '^dtoverlay=vc4-kms-v3d\$' '$CFG2'"
export PIVID_CONFIG_TXT="$CFG"

# bad args -> nonzero exit
"$HELPER" set composite bogus; chk "bad standard rejected" "[ $? -ne 0 ]"
"$HELPER" set bogus;           chk "bad mode rejected"     "[ $? -ne 0 ]"

rm -rf "$TMP"
if [ "$fail" = 0 ]; then echo "ALL PASS"; else echo "FAILURES"; exit 1; fi
