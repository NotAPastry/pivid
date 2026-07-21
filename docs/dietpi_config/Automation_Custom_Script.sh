#!/bin/bash
# Shared first-boot custom script (same for TX and RX).
# Copy to /boot/Automation_Custom_Script.sh on the flashed SD card.
#
# Scope: only OS hygiene + avahi. PiVid's runtime packages
# (rpicam-apps-lite, gstreamer, libgpiod3) are installed by
# scripts/deploy.sh's apt pre-flight instead -- that's the single
# source of truth for what each role needs, runs every deploy (catches
# drift), and fails loudly under the operator's eyes instead of
# silently at first boot.
#
# avahi-daemon MUST stay here: the deploy host needs `pivid-{tx,rx}.local`
# to resolve via mDNS *before* deploy.sh can ssh in to install anything.
#
# Log: /var/lib/dietpi/logs/dietpi-automation_custom_script.log

set -eu

#---------------------------------------------------------------
# 1. mDNS via avahi-daemon.
#---------------------------------------------------------------
# Bootstraps the deploy path: without this, pivid-tx.local /
# pivid-rx.local don't resolve and the deploy host can't reach the Pi
# by hostname.
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends avahi-daemon
systemctl unmask avahi-daemon.service 2>/dev/null || true
systemctl enable --now avahi-daemon.service

#---------------------------------------------------------------
# 2. Pin the kernel + bootloader.
#---------------------------------------------------------------
# The lr2021.ko shipped by `make deploy` is built against a frozen
# RPI_KERNEL_GIT_REF in docker/Dockerfile.rpi64. If the kernel bumps,
# vermagic stops matching and deploy fails. Hold whatever kernel /
# firmware packages are actually installed on this image.
#
# NOTE: this hold is a BACKSTOP, not the primary pin. This script runs
# "post-networking and post-DietPi install" (per dietpi.txt), i.e. AFTER
# first-run setup has already done its apt upgrade -- too late to stop a
# first-boot kernel bump. The real pin lives in the PRE script,
# Automation_Custom_PreScript.sh, which runs before that upgrade. Both must
# be copied to /boot on the flashed card. Keeping this loop here is
# harmless (apt-mark hold is idempotent) and covers a card flashed without
# the pre-script.
for pkg in \
    linux-image-rpi-v8 linux-image-rpi-2712 \
    linux-headers-rpi-v8 linux-headers-rpi-2712 \
    raspi-firmware rpi-eeprom
do
    if dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q '^install ok installed$'; then
        apt-mark hold "$pkg"
    fi
done

#---------------------------------------------------------------
# 3. Disable Bluetooth userspace.
#---------------------------------------------------------------
# dtoverlay=disable-bt in config.txt detaches the BT modem from the
# PL011 UART (freeing GPIO14/15 for the flight controller), but the
# Linux services will still try to start and complain.
systemctl disable --now hciuart.service bluetooth.service 2>/dev/null || true
systemctl mask          hciuart.service bluetooth.service 2>/dev/null || true

#---------------------------------------------------------------
# 4. Mask APT daily timers.
#---------------------------------------------------------------
# CONFIG_CHECK_APT_UPDATES=0 stops the DietPi-side check, but the
# systemd timers are independent and would still run apt-get update.
systemctl mask apt-daily.timer apt-daily-upgrade.timer 2>/dev/null || true

#---------------------------------------------------------------
# 5. Un-blacklist RPi camera / codec / VC shared-memory modules.
#---------------------------------------------------------------
# DietPi's headless preset writes /etc/modprobe.d/dietpi-disable_*
# files that blacklist bcm2835_isp, bcm2835_codec, vc_sm_cma, etc.
# systemd-modules-load.service honors blacklists and silently skips
# entries -- so the /etc/modules-load.d/ files that deploy.sh installs
# would no-op even though the kernel modules ship in /lib/modules.
#
# TX needs bcm2835-isp + bcm2835-codec + vc_sm_cma (libcamera ISP + H.264 encode).
# RX needs bcm2835-codec + vc_sm_cma (V4L2 H.264 decode).
# Removing all three blacklists on both roles is idempotent: the actual
# /etc/modules-load.d/ entries are role-specific, so unused modules
# simply don't load.
rm -f /etc/modprobe.d/dietpi-disable_rpi_camera.conf \
      /etc/modprobe.d/dietpi-disable_rpi_codec.conf \
      /etc/modprobe.d/dietpi-disable_vcsm.conf

exit 0
