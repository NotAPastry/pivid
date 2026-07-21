#!/usr/bin/env bash
# Build bcm2835-codec.ko (optionally patched) in the pivid-builder image.
#
# The kernel tree lives at /opt/rpi-linux INSIDE the image (pinned ref).
# We apply the in-repo patch to that tree, ensure the staging codec is =m,
# build the in-tree module via M=, and copy the .ko to bin/.
set -euo pipefail

IMAGE="pivid-builder:latest"
KDIR="/opt/rpi-linux"
MOD_REL="drivers/staging/vc04_services/bcm2835-codec"
PATCH="${PIVID_PATCH:-patches/bcm2835-codec-intra-refresh-slices.patch}"

mkdir -p bin

docker run --rm -v "$(pwd):/work" -w /work "$IMAGE" bash -euxc "
  cd '$KDIR'
  # Apply the trial patch (empty patch = stock build). -p1 from kernel root.
  if [ -s /work/$PATCH ]; then
      git apply --verbose /work/$PATCH
  fi
  # Ensure the staging codec builds as a module, then re-prepare.
  ./scripts/config --module CONFIG_VIDEO_CODEC_BCM2835
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
  # Build only the codec module (expect modpost 'undefined vchiq_mmal_*' warnings).
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M='$MOD_REL' modules
  cp '$MOD_REL/bcm2835-codec.ko' /work/bin/bcm2835-codec.ko
"
echo "built: bin/bcm2835-codec.ko"
# vermagic: prefer modinfo (Linux), fall back to strings (portable, e.g. macOS host).
# The .modinfo section stores it as "vermagic=..."; modinfo prints "vermagic:   ...".
( modinfo bin/bcm2835-codec.ko 2>/dev/null || strings bin/bcm2835-codec.ko ) \
  | grep -m1 -E '^vermagic[:=]' | sed 's/^vermagic[:=][[:space:]]*/vermagic: /' || true
