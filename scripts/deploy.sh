#!/usr/bin/env bash
# scripts/deploy.sh — push pivid binaries + LR2021 .ko + DT overlay to
# TX and RX Pi's.
#
# Required env (override on command line, e.g. TX_HOST=10.0.0.5 ./deploy.sh):
#   TX_HOST       — hostname or IP of the TX Pi   (default: pivid-tx)
#   RX_HOST       — hostname or IP of the RX Pi   (default: pivid-rx)
#   DEPLOY_USER   — ssh login user                (default: root)
#
# Pre-flight on host: `make all` must have produced bin/pivid-tx,
# bin/pivid-rx, bin/lr2021.ko, bin/lr2021.dtbo.
#
# First install requires one reboot to pick up the DT overlay; the script
# detects the missing /dev/radio0 and exits 0 with a clear "reboot then
# re-run" message instead of failing the service start.
set -euo pipefail

TX_HOST="${TX_HOST:-pivid-tx}"
RX_HOST="${RX_HOST:-pivid-rx}"
DEPLOY_USER="${DEPLOY_USER:-root}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="$REPO_ROOT/bin"
KO="$BIN_DIR/lr2021.ko"
DTBO="$BIN_DIR/lr2021.dtbo"
# Single patched VPU codec module (exposes resilience controls; neutral defaults).
# Recipe selection happens at encode time via the patched rpicam-vid's CLI flags.
CODEC_KO="$BIN_DIR/bcm2835-codec.ko"
RPICAM_VID="$BIN_DIR/rpicam-vid"

for f in "$BIN_DIR/pivid-tx" "$BIN_DIR/pivid-rx" "$BIN_DIR/pivid-config-agent" \
         "$BIN_DIR/pivid-caster" \
         "$KO" "$DTBO" "$CODEC_KO" "$RPICAM_VID"; do
    [[ -f "$f" ]] || { echo "missing: $f (run \`make all\` first)" >&2; exit 1; }
done
# librpicam_app.so.* is a versioned glob, not a fixed name -- check separately.
# Exclude the meson .symbols sidecar so a stray 0-byte file can't satisfy this.
ls "$BIN_DIR"/librpicam_app.so.* 2>/dev/null | grep -qv '\.symbols$' \
    || { echo "missing: $BIN_DIR/librpicam_app.so.* (run \`make rpicam\` first)" >&2; exit 1; }

# Vermagic of the built .ko — must match `uname -r` on each target Pi.
# Read the vermagic string directly from the .ko's .modinfo section so we
# don't need the Linux-only `modinfo` tool on the build host. The line
# looks like:  vermagic=6.6.51+rpt-rpi-v8 SMP preempt mod_unload aarch64
# We want the first whitespace-separated field, i.e. the kernel release.
KO_VERMAGIC="$(grep -a -o 'vermagic=[^[:cntrl:]]*' "$KO" \
                 | head -1 \
                 | sed 's/^vermagic=//' \
                 | awk '{print $1}')"
[[ -n "$KO_VERMAGIC" ]] || { echo "FATAL: could not extract vermagic from $KO" >&2; exit 1; }
echo "Built .ko vermagic: $KO_VERMAGIC"

# The patched codec .ko must share the kernel release, or modprobe rejects it
# and silently falls back to the stock in-tree module -> encoder controls gone.
# Catch a mismatch here rather than discover it on-air.
cv="$(grep -a -o 'vermagic=[^[:cntrl:]]*' "$CODEC_KO" \
        | head -1 | sed 's/^vermagic=//' | awk '{print $1}')"
[[ "$cv" == "$KO_VERMAGIC" ]] || {
    echo "FATAL: codec vermagic ($cv) != lr2021 ($KO_VERMAGIC)" >&2
    exit 1; }

# Role-specific runtime package sets. apt-get install is idempotent: already-
# installed packages are no-ops, missing ones get installed. Keep these in
# sync with docs/dietpi_config/{tx,rx}/Automation_Custom_Script.sh -- both
# paths must converge on the same end state.
TX_APT_PKGS=(rpicam-apps-lite libgpiod3 avahi-daemon hostapd dnsmasq)
RX_APT_PKGS=(
    libgpiod3
    avahi-daemon
    libavahi-client3
    libavahi-common3
    hostapd
    dnsmasq
    libsdl2-2.0-0
    libavcodec-extra
    libcairo2
    # EGL/GBM/Mesa runtime for the local HDMI player. SDL2's KMSDRM backend
    # dlopen()s libEGL.so.1 at runtime rather than linking it, so libsdl2-2.0-0
    # only *Recommends* the EGL stack -- and we install with
    # --no-install-recommends, which means a freshly flashed card gets none of
    # it. The player then crash-loops with "SDL_CreateWindow: EGL not
    # initialized" even though /dev/dri/card0 and the HDMI connector are fine.
    # libgl1-mesa-dri is the load-bearing one: without it
    # /usr/lib/*/dri/ is empty and Mesa has no driver to hand EGL.
    # (Long-lived RX cards had these pulled in by earlier ad-hoc installs,
    # which is why this only surfaced on a reflash.)
    libegl1
    libegl-mesa0
    libgles2
    libgl1-mesa-dri
    libgbm1
)

# Bring an already-deployed pivid.conf to the current schema: DELETE the dead
# rx/osd keys (the rx slim-down stopped reading them) and ensure the new
# FORWARD_TO_PC/AQM keys exist. Idempotent: deleting an absent line and
# ensuring a present key are both no-ops. Arg: $1 = the per-host $ssh prefix.
#
# NOTE: the PIVID_* names in the dead-key purge list and the PIVID_FEC_M source
# below are HISTORICAL literals -- they match keys written by OLD (pre-rename)
# pivid.conf files. The active-schema keys lost their PIVID_ prefix; renaming an
# existing operator config to the new keys is a SEPARATE manual step (see the
# rename migration one-liner in the rename commit / docs), intentionally NOT
# done here.
migrate_conf() {
    local ssh="$1"
    $ssh 'conf=/opt/pivid/pivid.conf; [ -f "$conf" ] || exit 0;
        # Drop dead keys left from the OSD-burn-in / rx-output-mode era.
        for k in PIVID_RX_MODE PIVID_RX_UDP_ADDR PIVID_RX_UDP_PORT \
                 PIVID_OSD_ENABLED PIVID_OSD_REENCODE_BITRATE PIVID_OSD_IDR_PERIOD \
                 PIVID_OSD_FONT_FACE PIVID_OSD_FONT PIVID_OSD_FONT_SMOOTH \
                 PIVID_CTL_RSSI_LOW PIVID_VID_LQ_LOW \
                 PIVID_FFMPEG_BIN; do
            sed -i "/^${k}=/d" "$conf";
        done;
        # Guarantee a trailing newline ONCE before any append (a file whose last
        # line lacks \n would fuse two keys on append).
        [ -s "$conf" ] && [ -n "$(tail -c1 "$conf")" ] && printf "\n" >> "$conf";
        # Rename the old single anchor-m key to the per-class anchor m, then drop
        # it. Preserve the operator value if present; default handled by ensure.
        old=$(sed -n "s/^PIVID_FEC_M=//p" "$conf" | head -n1);
        if [ -n "$old" ]; then
            grep -q "^FEC_M_ANCHOR=" "$conf" || printf "FEC_M_ANCHOR=%s\n" "$old" >> "$conf";
        fi;
        sed -i "/^PIVID_FEC_M=/d" "$conf";
        ensure() { grep -q "^$1=" "$conf" || printf "%s\n" "$2" >> "$conf"; };
        ensure IDR_PERIOD     "IDR_PERIOD=60";
        ensure FORWARD_TO_PC  "FORWARD_TO_PC=0";
        ensure PLAYER_LOCAL   "PLAYER_LOCAL=0";
        ensure AQM            "AQM=enforce";
        ensure FEC_M_ANCHOR   "FEC_M_ANCHOR=9";
        ensure FEC_M_REF      "FEC_M_REF=2";
        ensure FEC_K          "FEC_K=16";
        ensure OSD_KEYFRAME_MS   "OSD_KEYFRAME_MS=200";
        ensure OSD_VID_RSSI_CELL "OSD_VID_RSSI_CELL=1,1";
        ensure OSD_VID_LQ_CELL   "OSD_VID_LQ_CELL=2,1";'
}

deploy_one() {
    local host="$1" role="$2"          # role is "tx" or "rx"
    local bin="pivid-${role}"
    local svc="pivid-${role}.service"
    local ssh="ssh -o BatchMode=yes -o ConnectTimeout=10 ${DEPLOY_USER}@${host}"
    local scp="scp -o BatchMode=yes -o ConnectTimeout=10"

    echo "=== ${host} (${role}) ==="

    # 1. Vermagic pre-check. Compare the kernel release prefix; full vermagic
    #    also includes SMP/preempt flags but those rarely drift on a frozen
    #    image. If you see a mismatch, the kernel-source pin in
    #    docker/Dockerfile.rpi64 (RPI_KERNEL_GIT_REF) does not match the OS
    #    image on this Pi -- rebuild the image with the correct ref.
    local pi_kver
    pi_kver="$($ssh uname -r)"
    if [[ "$KO_VERMAGIC" != "$pi_kver"* ]]; then
        echo "FATAL: .ko built for '$KO_VERMAGIC' but $host runs '$pi_kver'." >&2
        echo "       Rebuild pivid-builder against the matching kernel ref." >&2
        exit 1
    fi

    # 2. APT pre-flight. Belt-and-braces against the first-boot custom script
    #    having been skipped or interrupted (we've seen real Pi's where
    #    AUTO_SETUP_APT_INSTALLS exited mid-install with .install_stage=2
    #    already written, leaving the system silently incomplete). apt-get
    #    is idempotent; this is a no-op when everything is already present.
    local pkgs
    # Indirect-array expansion via ${!var} needs bash 4+ -- not on macOS 3.2.
    case "$role" in
        tx) pkgs="${TX_APT_PKGS[*]}" ;;
        rx) pkgs="${RX_APT_PKGS[*]}" ;;
    esac
    echo "  apt pre-flight: ${pkgs}"
    if ! $ssh "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ${pkgs}" \
            >/dev/null; then
        echo "FATAL: apt-get install failed on ${host}." >&2
        echo "       Re-run interactively to see the error:" >&2
        echo "       ssh ${DEPLOY_USER}@${host} apt-get install ${pkgs}" >&2
        exit 1
    fi

    # 3. Stop service before replacing files (idempotent).
    $ssh "systemctl stop $svc 2>/dev/null || true"

    # 3b. Remove DietPi's camera/codec/vcsm blacklists if present.
    #     DietPi's headless preset writes /etc/modprobe.d/dietpi-disable_*
    #     files that blacklist bcm2835_isp, bcm2835_codec, vc_sm_cma, etc.
    #     systemd-modules-load.service honors those blacklists, so the
    #     /etc/modules-load.d/ entries below would silently no-op even
    #     though the kernel modules are present in /lib/modules.
    #     Automation_Custom_Script.sh handles this on first boot; we also
    #     do it here as an idempotent safety net for already-imaged Pi's
    #     and re-deploys after dietpi-config changes.
    $ssh 'rm -f /etc/modprobe.d/dietpi-disable_rpi_camera.conf \
                /etc/modprobe.d/dietpi-disable_rpi_codec.conf \
                /etc/modprobe.d/dietpi-disable_vcsm.conf'

    # 4. Userspace binary + systemd unit + canonical config. The daemon spawns
    #    rpicam-vid itself and reads config via --config; there is no run-*.sh.
    $scp "$BIN_DIR/$bin"                           "${DEPLOY_USER}@${host}:/tmp/$bin"
    $scp "$REPO_ROOT/target/etc/systemd/system/$svc"                 "${DEPLOY_USER}@${host}:/tmp/$svc"
    $scp "$REPO_ROOT/target/opt/pivid/pivid.conf"           "${DEPLOY_USER}@${host}:/tmp/pivid.conf"
    $ssh "install -d -m 0755 /opt/pivid \
       && install -m 0755 /tmp/$bin /opt/pivid/$bin \
       && install -m 0644 /tmp/$svc /etc/systemd/system/$svc \
       && ( test -f /opt/pivid/pivid.conf \
            || install -m 0644 /tmp/pivid.conf /opt/pivid/pivid.conf ) \
       `# migration: drop the pre-daemon wrapper scripts off older installs` \
       && rm -f /opt/pivid/run-tx.sh /opt/pivid/run-rx.sh \
       && rm /tmp/$bin /tmp/$svc /tmp/pivid.conf"

    # 4d. Migrate already-deployed confs to the current schema (idempotent:
    #     sed-delete the dead rx/osd keys + ensure PIVID_FORWARD_TO_PC/PIVID_AQM). Harmless
    #     on TX -- the dead keys simply aren't present there. Guarded by
    #     [ -f "$conf" ] inside migrate_conf.
    migrate_conf "$ssh"

    # 4a2. RX-only: install + enable pivid-caster (the distribution hub). It runs
    #      as its own always-on unit alongside pivid-rx; it never opens the radio,
    #      so it has no ConditionPathExists and starts regardless of radio state.
    #      Stop any prior instance (incl. a transient test unit) so it releases the
    #      loopback ports before we replace the binary.
    if [ "$role" = "rx" ]; then
        $ssh "systemctl stop pivid-caster 2>/dev/null || true"
        $scp "$BIN_DIR/pivid-caster"                  "${DEPLOY_USER}@${host}:/tmp/pivid-caster"
        $scp "$REPO_ROOT/target/etc/systemd/system/pivid-caster.service" "${DEPLOY_USER}@${host}:/tmp/pivid-caster.service"
        $ssh "install -m 0755 /tmp/pivid-caster /opt/pivid/pivid-caster \
           && install -m 0644 /tmp/pivid-caster.service /etc/systemd/system/pivid-caster.service \
           && rm /tmp/pivid-caster /tmp/pivid-caster.service \
           && systemctl daemon-reload \
           && systemctl enable --now pivid-caster \
           && systemctl restart pivid-caster \
           && systemctl status --no-pager pivid-caster | head -n 12"
    fi

    # 4a2b. RX-only: install the display-output helper the config agent execs
    #       (AGENT_DISPLAY_HELPER = /opt/pivid/pivid-display.sh) to switch the
    #       local output between HDMI and composite (PLAYER_COMPOSITE /
    #       PLAYER_TV_STANDARD). Without it, op_display_set has nothing to exec.
    if [ "$role" = "rx" ]; then
        $scp "$REPO_ROOT/target/opt/pivid/pivid-display.sh" "${DEPLOY_USER}@${host}:/tmp/pivid-display.sh"
        $ssh "install -m 0755 /tmp/pivid-display.sh /opt/pivid/pivid-display.sh \
           && rm -f /tmp/pivid-display.sh"
    fi

    # 4a3. RX-only: provision the local HDMI player runtime. The on/off DASHBOARD
    #      toggle (PLAYER_LOCAL) owns enable/disable, so we install the unit
    #      DISABLED here and never enable it. Runtime libs come from RX_APT_PKGS
    #      (above). We also ensure the KMS display overlay so the player has a
    #      DRM device. The binary itself is shipped only when `make player` built
    #      it (heavier SDL2/FFmpeg build); a plain radio deploy skips it.
    if [ "$role" = "rx" ]; then
        $scp "$REPO_ROOT/target/etc/systemd/system/pivid-player.service" "${DEPLOY_USER}@${host}:/tmp/pivid-player.service"
        $ssh "install -m 0644 /tmp/pivid-player.service /etc/systemd/system/pivid-player.service \
           && rm -f /tmp/pivid-player.service \
           && systemctl daemon-reload"
        # Ensure the KMS display driver so SDL2's KMSDRM backend has a device.
        # Idempotent; handles both modern (/boot/firmware) and legacy (/boot) paths.
        $ssh 'cfg=/boot/firmware/config.txt; [ -f "$cfg" ] || cfg=/boot/config.txt; \
              if [ -f "$cfg" ] && ! grep -q "^dtoverlay=vc4-kms-v3d" "$cfg"; then \
                  printf "\n# pivid: KMS display for the local HDMI player\ndtoverlay=vc4-kms-v3d\n" >> "$cfg"; \
                  echo "  added dtoverlay=vc4-kms-v3d to $cfg (REBOOT the RX once for the display)"; \
              fi'

        if [ -f "$BIN_DIR/pivid-player" ]; then
            $scp "$BIN_DIR/pivid-player" "${DEPLOY_USER}@${host}:/tmp/pivid-player"
            # If the player is currently enabled, restart it onto the new binary;
            # otherwise just stage it (the toggle will start it on demand).
            $ssh "install -m 0755 /tmp/pivid-player /opt/pivid/pivid-player \
               && rm -f /tmp/pivid-player \
               && ( systemctl is-enabled --quiet pivid-player 2>/dev/null \
                    && systemctl restart pivid-player || true )"
            echo "  installed /opt/pivid/pivid-player; service unit staged (toggle owns enable/disable)"
        else
            echo "  pivid-player.service installed (disabled). Binary NOT shipped:"
            echo "    build it with \`make player\` then re-run deploy.sh before turning"
            echo "    the dashboard 'Local HDMI player' toggle on."
        fi
    fi

    # 4b. Config portal: always-on config agent + network orchestrator +
    #     wifi-client helper + captive dnsmasq template + the config-mode systemd
    #     units. Installed on BOTH roles. The agent IS enabled at boot; the
    #     pivid-config-net unit is NOT enabled -- the agent starts it on demand on
    #     the 10 s config-button long-press. The stock dnsmasq.service auto-enables on install and
    #     would grab :53, so disable it; we run our own dedicated
    #     pivid-config-dnsmasq unit. (The stock hostapd.service ships masked on
    #     Debian; we never use it.)
    $scp "$BIN_DIR/pivid-config-agent"                     "${DEPLOY_USER}@${host}:/tmp/pivid-config-agent"
    $scp "$REPO_ROOT/target/etc/systemd/system/pivid-config-agent.service"   "${DEPLOY_USER}@${host}:/tmp/pivid-config-agent.service"
    $scp "$REPO_ROOT/target/opt/pivid/pivid-config-net.sh"          "${DEPLOY_USER}@${host}:/tmp/pivid-config-net.sh"
    $scp "$REPO_ROOT/target/opt/pivid/pivid-wifi-client.sh"         "${DEPLOY_USER}@${host}:/tmp/pivid-wifi-client.sh"
    $scp "$REPO_ROOT/target/opt/pivid/dnsmasq.conf.in"              "${DEPLOY_USER}@${host}:/tmp/dnsmasq.conf.in"
    $scp "$REPO_ROOT/target/etc/systemd/system/pivid-config-net.service"     "${DEPLOY_USER}@${host}:/tmp/pivid-config-net.service"
    $scp "$REPO_ROOT/target/etc/systemd/system/pivid-config-hostapd.service" "${DEPLOY_USER}@${host}:/tmp/pivid-config-hostapd.service"
    $scp "$REPO_ROOT/target/etc/systemd/system/pivid-config-dnsmasq.service" "${DEPLOY_USER}@${host}:/tmp/pivid-config-dnsmasq.service"
    $ssh "install -m 0755 /tmp/pivid-config-agent /opt/pivid/pivid-config-agent \
       && install -m 0644 /tmp/pivid-config-agent.service /etc/systemd/system/pivid-config-agent.service \
       && install -m 0755 /tmp/pivid-config-net.sh /opt/pivid/pivid-config-net.sh \
       && install -m 0755 /tmp/pivid-wifi-client.sh /opt/pivid/pivid-wifi-client.sh \
       && install -m 0644 /tmp/dnsmasq.conf.in /opt/pivid/dnsmasq.conf.in \
       && install -m 0644 /tmp/pivid-config-net.service      /etc/systemd/system/pivid-config-net.service \
       && install -m 0644 /tmp/pivid-config-hostapd.service  /etc/systemd/system/pivid-config-hostapd.service \
       && install -m 0644 /tmp/pivid-config-dnsmasq.service  /etc/systemd/system/pivid-config-dnsmasq.service \
       && ( rm -f /etc/systemd/system/pivid-config.target ) \
       && ( systemctl disable --now dnsmasq 2>/dev/null || true ) \
       && systemctl daemon-reload \
       && systemctl enable --now pivid-config-agent \
       && systemctl restart pivid-config-agent \
       && rm -f /tmp/pivid-config-agent /tmp/pivid-config-agent.service \
                /tmp/pivid-config-net.sh /tmp/pivid-wifi-client.sh /tmp/dnsmasq.conf.in \
                /tmp/pivid-config-net.service /tmp/pivid-config-hostapd.service \
                /tmp/pivid-config-dnsmasq.service"

    # 5. Kernel module: place into the standard out-of-tree location and run
    #    depmod so modprobe picks it up by name. The modprobe.d options conf is
    #    installed FIRST so any boot options are in place before modprobe. FLRC
    #    framing is dynamic, hardwired in the driver (no flrc_mode module knob),
    #    so the conf passes no flrc_mode= option; see target/etc/modprobe.d/lr2021.conf
    #    for the on-air A/B that justified dynamic. Both roles get the same conf.
    $scp "$KO" "${DEPLOY_USER}@${host}:/tmp/lr2021.ko"
    $scp "$REPO_ROOT/target/etc/modprobe.d/lr2021.conf" \
         "${DEPLOY_USER}@${host}:/tmp/lr2021-options.conf"
    $ssh "install -d -m 0755 /lib/modules/\$(uname -r)/extra \
       && install -m 0644 /tmp/lr2021.ko /lib/modules/\$(uname -r)/extra/lr2021.ko \
       && install -m 0644 /tmp/lr2021-options.conf /etc/modprobe.d/lr2021.conf \
       && rm /tmp/lr2021.ko /tmp/lr2021-options.conf \
       && depmod -a \
       && (modprobe -r lr2021 2>/dev/null || true) \
       && modprobe lr2021 \
       && lsmod | grep -E '^lr2021 '"

    # 6. Autoload at boot.
    $scp "$REPO_ROOT/target/etc/modules-load.d/lr2021.conf" \
         "${DEPLOY_USER}@${host}:/tmp/lr2021.conf"
    $ssh "install -m 0644 /tmp/lr2021.conf /etc/modules-load.d/lr2021.conf \
       && rm /tmp/lr2021.conf"

    # 6b. Autoload the V4L2 M2M codec at boot (both roles). The codec is a
    #     VC firmware service over VCHIQ rather than a DT-bound device, so
    #     udev has nothing to key off and won't auto-load it.
    #     RX: /dev/video10 (decoder) -> GStreamer v4l2h264dec.
    #     TX: /dev/video11 (encoder) -> rpicam-vid H.264 encode path.
    $scp "$REPO_ROOT/target/etc/modules-load.d/bcm2835-codec.conf" \
         "${DEPLOY_USER}@${host}:/tmp/bcm2835-codec.conf"
    $ssh "install -m 0644 /tmp/bcm2835-codec.conf /etc/modules-load.d/bcm2835-codec.conf \
       && rm /tmp/bcm2835-codec.conf \
       && (lsmod | grep -qE '^bcm2835_codec ' || modprobe bcm2835-codec)"

    # 6b-tx: TX-only -- install the SINGLE patched VPU codec module into
    #     /lib/modules/$(uname -r)/updates/ (depmod prioritises it over the
    #     in-tree module), plus the patched rpicam-vid binary + its matching
    #     librpicam_app.so. The encoder recipe is now chosen at encode time by
    #     rpicam-vid's CLI flags (built in C by pivid-tx; see common/camera_argv.c) -- no .ko swap, no
    #     select-encoder.sh. The RX keeps the stock in-tree codec (encode-side
    #     controls are inert on the decode path).
    if [ "$role" = "tx" ]; then
        $scp "$CODEC_KO" "${DEPLOY_USER}@${host}:/tmp/bcm2835-codec.ko"
        $scp "$RPICAM_VID" "${DEPLOY_USER}@${host}:/tmp/rpicam-vid"
        so_local=$(ls "$BIN_DIR"/librpicam_app.so.* 2>/dev/null | grep -v '\.symbols$' | head -1)
        so_base=$(basename "$so_local")
        $scp "$so_local" "${DEPLOY_USER}@${host}:/tmp/${so_base}"
        $ssh "set -e
           kdir=/lib/modules/\$(uname -r)/updates
           install -d -m 0755 \$kdir
           install -m 0644 /tmp/bcm2835-codec.ko \$kdir/bcm2835-codec.ko
           depmod -a
           # Install patched rpicam-vid over the apt one, and our librpicam_app.so
           # into the dir dpkg owns it in (ldconfig rebuilds the .so.1 symlink).
           libdir=\$(dirname \"\$(dpkg -L librpicam-app1 | grep -m1 'librpicam_app\.so\.[0-9]')\")
           [ -n \"\$libdir\" ] && [ \"\$libdir\" != . ] || { echo 'FATAL: librpicam-app1 not installed on this Pi' >&2; exit 1; }
           install -m 0755 /tmp/rpicam-vid \"\$(command -v rpicam-vid)\"
           install -m 0644 /tmp/${so_base} \"\$libdir/${so_base}\"
           ldconfig
           rm -f /tmp/bcm2835-codec.ko /tmp/rpicam-vid /tmp/${so_base}
           # Remove stale select-encoder.sh leftovers: its systemd drop-in pins
           # PIVID_INTRA/PIVID_SPS_REINJECT_MS in the service env, which would
           # override the recipe vars the daemon now derives from pivid.conf (e.g. keep SPS_REINJECT=0 for
           # the gdr recipe). Also drop the old encoders dir + switch script.
           rm -f /etc/systemd/system/pivid-tx.service.d/encoder.conf \
                 /opt/pivid/select-encoder.sh /opt/pivid/encoder-recipe
           rm -rf /opt/pivid/encoders
           systemctl daemon-reload
           # Reload codec so the single module's controls are live, then restart
           # pivid-tx (no-op on a first install: gated by ConditionPathExists).
           systemctl stop pivid-tx 2>/dev/null || true
           rmmod bcm2835_codec 2>/dev/null || true
           modprobe bcm2835-codec
           systemctl start pivid-tx 2>/dev/null || true"
    fi

    # 6c. TX-only: autoload the VC4 ISP service at boot. Same shape as
    #     6b -- it's a VCHIQ firmware service with no modalias, so udev
    #     can't auto-load it. Without it the ISP V4L2 nodes
    #     (/dev/video13..16) are missing and libcamera's rpi/vc4
    #     pipeline handler refuses to initialise, which makes
    #     rpicam-apps emit the misleading "only supports the Raspberry
    #     Pi platforms" error even after the camera sensor binds.
    if [ "$role" = "tx" ]; then
        $scp "$REPO_ROOT/target/etc/modules-load.d/bcm2835-isp.conf" \
             "${DEPLOY_USER}@${host}:/tmp/bcm2835-isp.conf"
        $ssh "install -m 0644 /tmp/bcm2835-isp.conf /etc/modules-load.d/bcm2835-isp.conf \
           && rm /tmp/bcm2835-isp.conf \
           && (lsmod | grep -qE '^bcm2835_isp ' || modprobe bcm2835-isp)"
    fi

    # 6d. TX-only: dma-heap name compatibility shim for rpicam-apps.
    #     Kernel 6.18 + cma=128M registers the CMA heap as
    #     /dev/dma_heap/default_cma_region, but rpicam-apps 1.12.0 only
    #     looks for /dev/dma_heap/{vidbuf_cached,linux,cma}. Install a
    #     systemd-tmpfiles entry to symlink linux,cma -> default_cma_region
    #     on every boot, and run --create to make the symlink present
    #     immediately (without waiting for the next reboot).
    if [ "$role" = "tx" ]; then
        $scp "$REPO_ROOT/target/etc/tmpfiles.d/pivid-dma-heap-compat.conf" \
             "${DEPLOY_USER}@${host}:/tmp/pivid-dma-heap-compat.conf"
        $ssh "install -m 0644 /tmp/pivid-dma-heap-compat.conf \
                       /etc/tmpfiles.d/pivid-dma-heap-compat.conf \
           && rm /tmp/pivid-dma-heap-compat.conf \
           && systemd-tmpfiles --create /etc/tmpfiles.d/pivid-dma-heap-compat.conf"
    fi

    # 7. Device-tree overlay. Copy .dtbo into the firmware's overlay
    #    directory and ensure config.txt loads it on boot. Idempotent:
    #    the grep-then-append adds the dtoverlay= line only if not present.
    #    The overlay takes effect at the NEXT reboot -- on a fresh image,
    #    /dev/radio0 will not exist yet even after modprobe.
    #
    #    Trailing-newline guard: DietPi ships config.txt without a final
    #    newline in some builds, so a naive `echo ... >> config.txt` glues
    #    `dtoverlay=lr2021` to the end of the previous line and the
    #    bootloader silently ignores both. Ensure the file ends with \n
    #    before we append.
    $scp "$DTBO" "${DEPLOY_USER}@${host}:/tmp/lr2021.dtbo"
    $ssh "install -m 0644 /tmp/lr2021.dtbo /boot/firmware/overlays/lr2021.dtbo \
       && rm /tmp/lr2021.dtbo \
       && (grep -qE '^dtoverlay=lr2021\$' /boot/firmware/config.txt \
           || { [ -z \"\$(tail -c1 /boot/firmware/config.txt)\" ] \
                  || echo >> /boot/firmware/config.txt; \
                echo 'dtoverlay=lr2021' >> /boot/firmware/config.txt; })"

    # 8. First-install reboot gate. Until the overlay has taken effect,
    #    /dev/radio0 does not exist and the service will fail
    #    ConditionPathExists=. Reboot the Pi, wait for it to come back,
    #    then continue. boot_id is read pre-reboot so we can confirm we're
    #    talking to the new kernel (not a stale SSH socket) before
    #    re-checking /dev/radio0.
    if ! $ssh "[ -c /dev/radio0 ]"; then
        echo "  /dev/radio0 not present yet; rebooting ${host} to apply DT overlay..."
        local pre_boot_id new_boot_id deadline
        pre_boot_id="$($ssh 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)"
        # SSH session is killed by the reboot; ignore the resulting error.
        $ssh 'systemctl reboot' >/dev/null 2>&1 || true
        deadline=$(( SECONDS + 180 ))
        new_boot_id=""
        while (( SECONDS < deadline )); do
            sleep 5
            new_boot_id="$($ssh 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null || true)"
            [[ -n "$new_boot_id" && "$new_boot_id" != "$pre_boot_id" ]] && break
        done
        if [[ -z "$new_boot_id" || "$new_boot_id" == "$pre_boot_id" ]]; then
            echo "FATAL: ${host} did not come back within 180s after reboot." >&2
            exit 1
        fi
        echo "  ${host} back up (new boot_id)."
        if ! $ssh "[ -c /dev/radio0 ]"; then
            echo "FATAL: /dev/radio0 still missing after reboot on ${host}." >&2
            echo "  Check overlay load: ssh ${DEPLOY_USER}@${host} 'dmesg | grep -iE lr2021'" >&2
            echo "  Check config.txt:   ssh ${DEPLOY_USER}@${host} 'grep lr2021 /boot/firmware/config.txt'" >&2
            exit 1
        fi
    fi

    # 9. Enable + start the service.
    $ssh "systemctl daemon-reload \
       && systemctl enable --now $svc \
       && systemctl status --no-pager $svc | head -n 20"
}

# Which roles to deploy this run. Default is both (unchanged behaviour);
# set DEPLOY_ROLES="rx" (or "tx") to scope a single-Pi deploy.
DEPLOY_ROLES="${DEPLOY_ROLES:-tx rx}"
[[ " $DEPLOY_ROLES " == *" tx "* ]] && deploy_one "$TX_HOST" tx
[[ " $DEPLOY_ROLES " == *" rx "* ]] && deploy_one "$RX_HOST" rx

echo
echo "Done. Tail logs with:"
[[ " $DEPLOY_ROLES " == *" tx "* ]] && \
    echo "  ssh ${DEPLOY_USER}@${TX_HOST} journalctl -fu pivid-tx"
[[ " $DEPLOY_ROLES " == *" rx "* ]] && \
    echo "  ssh ${DEPLOY_USER}@${RX_HOST} journalctl -fu pivid-rx"
