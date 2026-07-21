#!/usr/bin/env bash
# Build patched rpicam-vid + librpicam_app.so for the TX Pi in the
# pivid-rpicam-builder image. The rpicam-apps source lives at /opt/rpicam-apps
# inside the image (pinned to v1.12.0). We apply the encoder-flags patch, cross-
# compile for arm64, and copy the drop-in artifacts to bin/.
set -euo pipefail

IMAGE="pivid-rpicam-builder:latest"
SRC="/opt/rpicam-apps"
PATCH="${PIVID_RPICAM_PATCH:-patches/rpicam-apps-encoder-flags.patch}"

mkdir -p bin

docker run --rm -v "$(pwd):/work" -w "$SRC" "$IMAGE" bash -euxc "
  cd '$SRC'
  git checkout -- .
  if [ -s /work/$PATCH ]; then git apply --verbose /work/$PATCH; fi
  # Fresh container: no pre-existing build/, so plain 'meson setup' (NOT --wipe).
  meson setup build --cross-file /opt/cross-aarch64.txt \
    -Denable_libav=disabled -Denable_drm=disabled -Denable_egl=disabled \
    -Denable_qt=disabled -Denable_opencv=disabled -Denable_tflite=disabled -Denable_hailo=disabled
  ninja -C build apps/rpicam-vid    # target is apps/rpicam-vid, NOT rpicam-vid
  cp build/apps/rpicam-vid /work/bin/rpicam-vid
  # Copy the real .so directly from build/ root (meson places it there).
  # Avoid the .p subdir and .symbols files by limiting to maxdepth 1.
  so=\$(find build -maxdepth 1 -name 'librpicam_app.so.*' -type f | head -1)
  cp \"\$so\" /work/bin/\$(basename \"\$so\")
"
echo 'built:'; file bin/rpicam-vid; ls -l bin/librpicam_app.so.* 2>/dev/null || true
