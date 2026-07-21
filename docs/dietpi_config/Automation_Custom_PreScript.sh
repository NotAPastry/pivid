#!/bin/bash
# Shared first-boot PRE script (same for TX and RX).
# Copy to /boot/Automation_Custom_PreScript.sh on the flashed SD card.
#
# WHY THIS FILE EXISTS
# --------------------
# PiVid ships a precompiled out-of-tree lr2021.ko built against a frozen
# kernel (RPI_KERNEL_GIT_REF in docker/Dockerfile.rpi64). Linux enforces a
# vermagic check at modprobe time, so the kernel on the Pi must stay at
# EXACTLY the version the image shipped with -- scripts/deploy.sh aborts
# with a FATAL if `uname -r` drifts from the .ko's vermagic.
#
# Automation_Custom_Script.sh (the POST script) already runs `apt-mark hold`
# on the kernel packages, but per DietPi's own dietpi.txt documentation it
# executes "post-networking and post-DietPi install" -- i.e. at the END of
# first-run setup, AFTER DietPi has already run its apt update/upgrade. On a
# fresh image that upgrade bumps the kernel before the hold is ever applied,
# and the very first `make deploy` then fails the vermagic pre-check.
#
# This PRE script runs "pre-networking and pre-DietPi install", which is the
# only hook that lands BEFORE that upgrade. `apt-mark hold` is a purely local
# dpkg state change (no network needed), so it works fine this early.
#
# Log: /var/lib/dietpi/logs/dietpi-automation_custom_prescript.log

set -eu

echo "[pivid-prescript] pinning kernel/firmware BEFORE first-run apt upgrade"
echo "[pivid-prescript] running kernel: $(uname -r)"

#---------------------------------------------------------------
# Hold every installed kernel / firmware package.
#---------------------------------------------------------------
# Discovered by glob rather than a fixed list so this survives a different
# board flavour in the image (rpi-v8 on Pi 3/4/Zero 2W, rpi-2712 on Pi 5)
# without needing an edit. dpkg-query prints one "name status" pair per line;
# we hold only those actually installed.
held=0
while read -r pkg status; do
    [ "$status" = "installed" ] || continue
    apt-mark hold "$pkg" && held=$((held + 1))
done <<EOF
$(dpkg-query -W -f='${Package} ${Status}\n' \
      'linux-image-*' 'linux-headers-*' 'raspi-firmware' 'rpi-eeprom' \
      2>/dev/null | awk '{print $1, $4}' || true)
EOF

echo "[pivid-prescript] held ${held} kernel/firmware package(s)"

#---------------------------------------------------------------
# Belt-and-braces: APT pin as well as the dpkg hold.
#---------------------------------------------------------------
# `apt-mark hold` is respected by apt-get upgrade/dist-upgrade, but a pin
# makes the intent explicit and survives an accidental `apt-mark unhold`
# by a later operator. Priority < 0 makes the candidate uninstallable.
install -d -m 0755 /etc/apt/preferences.d
cat > /etc/apt/preferences.d/pivid-kernel-pin <<'PIN'
# PiVid: the shipped lr2021.ko is built against this image's exact kernel.
# Upgrading it breaks the modprobe vermagic check. Remove this file (and
# `apt-mark unhold`) only when deliberately rebuilding pivid-builder against
# a new kernel ref -- see docs/BUILD.md "Kernel pinning model".
Package: linux-image-* linux-headers-* raspi-firmware rpi-eeprom
Pin: release *
Pin-Priority: -1
PIN

echo "[pivid-prescript] wrote /etc/apt/preferences.d/pivid-kernel-pin"
echo "[pivid-prescript] done"

exit 0
