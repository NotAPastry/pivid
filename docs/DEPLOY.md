# Deploying PiVid v2.1

This guide walks through installing the build artifacts on a TX and RX
Pi, then operating the link.

Starting from a blank SD card? Do [SD_CARD.md](SD_CARD.md) first — it covers
flashing the DietPi image and applying `docs/dietpi_config/`. This guide assumes
both Pis already boot and answer to `pivid-tx.local` / `pivid-rx.local`.

## Target system assumptions

- **DietPi 9.x on Debian trixie** (or any RPi-firmware-based trixie
  image) on Raspberry Pi Zero 2W, Pi 3, Pi 4 — any board the pinned
  kernel supports.
- Frozen kernel image. The deploy script verifies vermagic matches; if
  the OS upgrades the kernel underneath you, the next deploy will fail
  loudly and you must rebuild the `pivid-builder` image against the
  new kernel ref. See [BUILD.md](BUILD.md#kernel-pinning-model).
- Root ssh access via key (no password prompts — `ssh -o BatchMode=yes`).
- Runtime packages: installed by `Automation_Custom_Script.sh` at
  first boot, AND re-verified by `scripts/deploy.sh` via an apt
  pre-flight before pushing binaries (defense in depth — see "Package
  install model" below).

## Pre-flight

```sh
# Confirm both Pi's are reachable
ssh root@pivid-tx uname -r
ssh root@pivid-rx uname -r

# Confirm your built .ko matches both Pi's kernels
grep -ao 'vermagic=[^[:cntrl:]]*' bin/lr2021.ko | head -1
```

If the vermagic prefix differs from `uname -r`, rebuild the
`pivid-builder` image — see [BUILD.md](BUILD.md#kernel-pinning-model).

## One-shot install

```sh
make deploy
# or with overrides:
TX_HOST=<tx-pi-ip> RX_HOST=<rx-pi-ip> DEPLOY_USER=pi make deploy
```

`scripts/deploy.sh` does the following on each Pi:

1. Reads vermagic from the local `.ko` and compares against
   `ssh ... uname -r`; aborts if mismatched.
2. **APT pre-flight.** `ssh ... apt-get install -y` the role's runtime
   packages (TX: `rpicam-apps-lite libgpiod3 avahi-daemon`; RX:
   `gstreamer1.0-{tools,plugins-base,plugins-good,plugins-bad}
   libgpiod3 avahi-daemon`). apt is idempotent — no-op when already
   present, recovers when not. Belt-and-braces for cases where the
   first-boot custom script didn't run or got interrupted.
3. `systemctl stop pivid-<role>` (idempotent — ok if not yet installed).
4. `scp` the userspace binary, the run-script wrapper, and the systemd
   unit; `install` them into `/opt/pivid/` and `/etc/systemd/system/`.
5. `scp` the `.ko` to `/lib/modules/$(uname -r)/extra/`, run
   `depmod -a`, `modprobe` the driver.
6. Install `/etc/modules-load.d/lr2021.conf` for autoload at boot.
7. `scp` the `.dtbo` to `/boot/firmware/overlays/`, append
   `dtoverlay=lr2021` to `/boot/firmware/config.txt` if not already
   present.
8. **Check `/dev/radio0` exists.** If it doesn't (first install — the
   overlay only takes effect at boot), exit 0 with a "reboot then
   re-run deploy" message. If it exists, continue to step 9.
9. `systemctl daemon-reload && systemctl enable --now pivid-<role>`.

## Package install model

PiVid splits package installs along a clear boundary:

| Layer | Where | Installs |
|---|---|---|
| Kernel pin | `docs/dietpi_config/Automation_Custom_PreScript.sh` (one shared file) | nothing — holds/pins the kernel *before* first-run apt upgrade |
| OS bootstrap | `docs/dietpi_config/Automation_Custom_Script.sh` (one shared file) | only `avahi-daemon` + kernel-pin backstop / service masks |
| Application runtime | `scripts/deploy.sh` apt pre-flight | `rpicam-apps-lite`, `libgpiod3` (TX); `gstreamer1.0-{tools,plugins-base,plugins-good,plugins-bad}`, `libgpiod3` (RX) |

**Why this split:**

- `avahi-daemon` MUST be installed by the SD-card-flashed script —
  without it, `pivid-{tx,rx}.local` won't resolve and deploy.sh can't
  ssh in to install anything else.
- Everything else is installed by deploy.sh at deploy-time, where it
  runs under the operator's eyes with live error output. apt-get is
  idempotent, so it's a no-op on re-deploys.
- Single source of truth: the package list lives in deploy.sh only.
  Bump it once, every Pi picks up the change on the next deploy.

**Why not `AUTO_SETUP_APT_INSTALLS=` for the runtime packages?**
DietPi writes the `.install_stage=2` "setup complete" marker BEFORE
that variable's install runs. A network blip, ramlog rotation, or
reboot mid-install leaves the system looking healthy (stage 2) but
missing packages, with no automatic retry on subsequent boots. We saw
this on both a real TX and RX Pi during commissioning — that's what
motivated this model. Both `dietpi.txt` files in this repo therefore
have `AUTO_SETUP_APT_INSTALLS=` empty.

## Kernel pinning on a freshly flashed card

The precompiled `lr2021.ko` only loads against the exact kernel it was
built for (vermagic check), so **the Pi must stay on the kernel its
image shipped with**. Copy BOTH scripts to the boot partition before
first boot (this is step 2 of [SD_CARD.md](SD_CARD.md)):

```sh
cp docs/dietpi_config/Automation_Custom_PreScript.sh /Volumes/bootfs/
cp docs/dietpi_config/Automation_Custom_Script.sh    /Volumes/bootfs/
```

Order matters, and only the pre-script actually prevents the bump:

| Hook | DietPi runs it | Effect on the kernel |
|---|---|---|
| `Automation_Custom_PreScript.sh` | pre-networking, **before** first-run setup | holds + APT-pins the kernel → first-run `apt upgrade` keeps it back |
| `Automation_Custom_Script.sh` | **after** first-run setup completes | idempotent backstop; too late on its own |

If you flash a card with only the post-script, DietPi's first-run
`apt update && upgrade` bumps the kernel before anything holds it, and
the first `make deploy` aborts:

```
FATAL: .ko built for '<X>' but <host> runs '<Y>'.
```

**Recovering a card that already booted and upgraded.** Either
reinstall the kernel version matching the `.ko` and hold it:

```sh
ssh root@<host> 'uname -r'                                  # what it drifted to
ssh root@<host> 'apt-cache madison linux-image-rpi-v8'      # find the image version
ssh root@<host> 'apt-get install -y --allow-downgrades linux-image-rpi-v8=<ver> \
                 && apt-mark hold linux-image-rpi-v8 && reboot'
```

…or rebuild `pivid-builder` against the new kernel ref and re-`make all`
(see [BUILD.md](BUILD.md#kernel-pinning-model)). Pinning back is
usually faster; rebuilding is the right call if you intend to track the
newer kernel going forward.

## First install requires one reboot

The DT overlay binds `compatible = "pivid,lr2021"` to `spi0.0`. RPi
firmware loads overlays at boot — there's no runtime apply. So on a
fresh image:

```sh
make deploy
# → "/dev/radio0 not present on pivid-tx yet. ... reboot the Pi"
ssh root@pivid-tx reboot
ssh root@pivid-rx reboot
# wait for reboot
make deploy
# → both services come up
```

After the second `make deploy`, both services should be `active`
within a few seconds.

## Service management

```sh
ssh root@pivid-tx systemctl status pivid-tx
ssh root@pivid-tx systemctl restart pivid-tx
ssh root@pivid-tx journalctl -fu pivid-tx
```

## Configuration (`/opt/pivid/pivid.conf`)

All tunables live in one canonical, shell-sourceable file:
`/opt/pivid/pivid.conf` (`PIVID_*=value`, `#` comments). It is the single
source of truth — every daemon parses it (`common/settings.c`), and it is
shell-sourceable for the helper scripts. There is **no** per-unit `systemd Environment=` override
anymore; edit the file and restart. `deploy.sh` installs the default
`target/opt/pivid/pivid.conf` only if the device has none, so field edits survive
re-deploys.

```sh
ssh root@pivid-tx vi /opt/pivid/pivid.conf
ssh root@pivid-tx systemctl restart pivid-tx     # camera/encoder/fec/rx changes
```

Selected keys (full default file: `target/opt/pivid/pivid.conf`; apply tiers:
`common/config_schema.c`):

| Key                    | Default        | Note                                            |
|------------------------|----------------|-------------------------------------------------|
| `PIVID_BINDING_PHRASE` | `pivid-default`| Pairing phrase → derived sync word (see below)  |
| `PIVID_WIDTH`/`_HEIGHT`| 640 / 480      | rpicam-vid `--width`/`--height`                 |
| `PIVID_FPS`            | 30             | rpicam-vid `--framerate`                        |
| `PIVID_BITRATE`        | 850000         | rpicam-vid `--bitrate` (bits/sec)               |
| `PIVID_ENCODER_RECIPE` | `gdr`          | `gdr` (CBR intra-refresh) or `droppable` (VBR)  |
| `PIVID_INTRA`          | 30             | IDR period (frames) — recovery anchor           |
| `PIVID_FEC_M`/`_K`     | 2 / 16         | RS-FEC parity / soft block bound                |
| `PIVID_RX_MODE`        | `display`      | `display` → GStreamer/kmssink; `udp` → raw socket |
| `PIVID_RX_UDP_ADDR`    | 127.0.0.1      | Only used when `PIVID_RX_MODE=udp`              |
| `PIVID_RX_UDP_PORT`    | 5000           | Only used when `PIVID_RX_MODE=udp`             |

The FLRC `PIVID_FLRC_*` keys (over-the-air framing) and `PIVID_BINDING_PHRASE`
**must match on both ends**. `PIVID_RF_FREQUENCY`/`PIVID_RF_TX_POWER` are
read-only (driver-hardcoded in `lr2021_init.c`); editing them has no effect yet.

The MSP DisplayPort OSD keys (`PIVID_OSD_*`) and the Betaflight FC setup are
documented separately — see [MSP DisplayPort OSD](MSP_DISPLAYPORT_OSD.md).

## Pairing (binding phrase)

Pairing is ELRS-style and deterministic — **no over-the-air handshake, no bind
button procedure**. The sync word is derived as
`nonzero(first 4 bytes of SHA-256(PIVID_BINDING_PHRASE))`
(`common/phrase_sync.c`), so the **same phrase on both ends → same 32-bit sync
word**. To pair (or to deconflict from another nearby LR2021 pair), set an
identical phrase on both Pis and restart:

```sh
ssh root@pivid-tx 'sed -i "s/^PIVID_BINDING_PHRASE=.*/PIVID_BINDING_PHRASE=\"tall-orange-falcon\"/" /opt/pivid/pivid.conf && systemctl restart pivid-tx'
ssh root@pivid-rx 'sed -i "s/^PIVID_BINDING_PHRASE=.*/PIVID_BINDING_PHRASE=\"tall-orange-falcon\"/" /opt/pivid/pivid.conf && systemctl restart pivid-rx'
```

Each daemon logs the derived word at startup
(`pivid-tx: started, sync_word=0x...`). The phrase persists, so a reboot
re-derives the same word and re-pairs unattended.

Verify with `tests/smoke_test.sh pair` — it compares `PIVID_BINDING_PHRASE`
across both Pis and asserts the daemon-logged derived sync words match.

## Hardware verification

```sh
make smoke                    # static + stream checks
tests/smoke_test.sh static    # just per-Pi sanity
tests/smoke_test.sh stream    # just data-flow check
tests/smoke_test.sh pair      # binding-phrase + derived sync_word match
```

What each check does is documented in detail in `tests/smoke_test.sh`
(see the file header). Exit code is non-zero on any assertion failure
so it can be wired into a pre-flight check.

## Troubleshooting

| Symptom | Likely cause | What to check |
|---|---|---|
| `make deploy` aborts with "FATAL: .ko built for X but $host runs Y" | Kernel-pin mismatch | `RPI_KERNEL_GIT_REF` in Dockerfile vs `uname -r` on the Pi. See [BUILD.md](BUILD.md#kernel-pinning-model). |
| `/dev/radio0` missing after reboot | DT overlay didn't apply | `grep dtoverlay=lr2021 /boot/firmware/config.txt` (should be present); `dmesg \| grep lr2021` (should show probe success). |
| Service stuck `activating` then `failed`, `journalctl` shows `gpiod_to_irq` errors | DT overlay didn't bind the DIO9 GPIO line to the driver | Re-deploy the `.dtbo`. Confirm `cat /sys/kernel/debug/gpio` shows GPIO27 owned by `lr2021-dio9`. |
| TX service in restart-loop | `rpicam-vid` can't open the camera | `journalctl -u pivid-tx -n 50` — look for the rpicam-vid error. Common: camera not enabled in `raspi-config`, ribbon cable disconnected. |
| `pivid-rx: nals=N` not advancing | Sync-word mismatch, or radio link broken | Compare `PIVID_BINDING_PHRASE` in both `pivid.conf` files (must be identical); confirm the daemon-logged `sync_word=0x...` matches on both. Check antennas, range, RF noise. |
| `pivid-tx: dropped_ioctl` growing | Driver returning `-EAGAIN` (write rejected) | `dmesg \| grep lr2021` — look for SPI / BUSY-timeout / IRQ errors. The driver only emits `-EAGAIN` when the TX ring is full AND the workqueue can't drain it fast enough; this usually means SPI bus problems. |
| `pivid-rx: torn=N` growing | Kernel ring producer outran consumer | The RX daemon isn't draining fast enough. Check CPU on the RX Pi — if `pivid-rx` is at 100 %, the GStreamer pipeline is the bottleneck (try `RX_MODE=udp` to remove the decode/display stage). |
| `pivid-rx: seq_drop` growing | Packet loss over the air | Expected at the edge of range or under interference. H.264 IDR (every 30 frames at default) is the recovery anchor; visual freezes should resolve within ~1 s. |
| Daemons paired but sync words differ | `PIVID_BINDING_PHRASE` not identical on both ends (trailing space, typo, quotes) | Diff the phrase line in both `pivid.conf` files exactly; they must be byte-identical. Restart both daemons after fixing. |
| All daemons running, no video on display | KMS device not available, or v4l2h264dec not installed | `ls /dev/dri/` (should show `card0`); `gst-inspect-1.0 v4l2h264dec` (should print plugin details). Falling back to `RX_MODE=udp` and pointing VLC at the RX Pi rules out the GStreamer pipeline. |
| RX boots with no `/dev/video10`, `v4l2h264dec` missing from `gst-inspect-1.0`, dmesg shows `vc_sm_cma_vchi_init: failed to open VCHI service (-22)` | Firmware loader picked `start_cd.elf` (cut-down VPU, no codec) because `gpu_mem` is too small | `vcgencmd version` last line should end `(start)` not `(start_cd)`. Fix: bump `gpu_mem_512` ≥ 76 in `/boot/firmware/config.txt` and reboot. |
| TX boots with `start.elf` firmware but `rpicam-hello --list-cameras` says "No cameras available", dmesg shows `imxNNN ... Error writing reg 0x3000: -5` / `Could not initialize device` | Sensor ACKs its I2C address but the first register write fails. In practice this is **almost always a bad / poorly seated CSI ribbon** — the I2C lines on a CSI ribbon are very flex-sensitive and a marginal cable can ACK the address byte yet corrupt subsequent bytes. Less commonly: master-clock or power-sequencing mismatch with the default RPi dtoverlay (some third-party / Waveshare boards). | First swap the ribbon and re-seat both latches. If the failure persists with a known-good cable, then check the camera vendor's wiki for a required `dtoverlay=` line and parameters (often a non-default `clock-frequency=`). If the dmesg sensor-probe line is absent entirely, it's a physical disconnection. |
| TX `rpicam-hello --version` or `--list-cameras` errors with "ERROR: rpicam-apps currently only supports the Raspberry Pi platforms" even though `/dev/video0` exists and the sensor subdev is bound | `bcm2835-isp` (VC4 ISP firmware service over VCHIQ) isn't loaded — it has no DT modalias so udev can't auto-load it, and libcamera's `rpi/vc4` pipeline handler refuses to initialise without it. The platform-probe error message is misleading; the platform check is really "did libcamera find a usable pipeline?" | `lsmod \| grep bcm2835_isp` should be non-empty. `ls /dev/video1[3-6]` should show the ISP nodes. Fix: `modprobe bcm2835-isp` for the immediate session; the deploy script installs `/etc/modules-load.d/bcm2835-isp.conf` so this persists across reboots. |
| `dtoverlay=lr2021` in `config.txt` is appended onto the end of the previous line (e.g. `dtoverlay=disable-btdtoverlay=lr2021`) | DietPi's `config.txt` ships without a final newline; older `deploy.sh` versions didn't guard for that | Current `deploy.sh` has a trailing-newline guard. If you see this on a Pi, edit `config.txt` to split the line, reboot. |

## Resetting pairing

To go back to the default pairing, set the default phrase on both ends:

```sh
ssh root@pivid-tx 'sed -i "s/^PIVID_BINDING_PHRASE=.*/PIVID_BINDING_PHRASE=\"pivid-default\"/" /opt/pivid/pivid.conf && systemctl restart pivid-tx'
ssh root@pivid-rx 'sed -i "s/^PIVID_BINDING_PHRASE=.*/PIVID_BINDING_PHRASE=\"pivid-default\"/" /opt/pivid/pivid.conf && systemctl restart pivid-rx'
```

## Uninstall

```sh
ssh root@pivid-tx 'systemctl disable --now pivid-tx; \
                    rm -rf /opt/pivid /etc/systemd/system/pivid-tx.service \
                           /etc/modules-load.d/lr2021.conf \
                           /lib/modules/$(uname -r)/extra/lr2021.ko \
                           /boot/firmware/overlays/lr2021.dtbo; \
                    sed -i "/^dtoverlay=lr2021$/d" /boot/firmware/config.txt; \
                    depmod -a; \
                    modprobe -r lr2021'
# repeat for pivid-rx
```

A subsequent reboot fully removes the LR2021 from device-tree.
