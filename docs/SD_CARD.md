# Preparing an SD card

Two cards are needed, one per Pi: a TX (camera) and an RX (receiver). They use
the same image and differ only in which files from `docs/dietpi_config/` you
copy onto the boot partition.

When you finish here, both Pis boot, join WiFi, and answer to
`pivid-tx.local` / `pivid-rx.local`. Then go to [DEPLOY.md](DEPLOY.md) to push
the binaries.

## What you need

- **`DietPi_RPi234-ARMv8-Trixie.img.xz`** from
  <https://dietpi.com/#download> (the RPi 2/3/4/Zero 2 W ARMv8 Trixie image).
  The Pi Zero 2 W uses this image, not the RPi1/Zero image.
- An SD-card writer: [Raspberry Pi Imager](https://www.raspberrypi.com/software/),
  [balenaEtcher](https://etcher.balena.io/), or `dd`. Imager can write `.xz`
  directly — do **not** use its "customise OS" prompt, since DietPi ignores it
  and it will overwrite the files you are about to copy.
- 4 GB card minimum; 8 GB or more recommended.

> **Kernel pinning matters here.** The precompiled `lr2021.ko` only loads
> against the exact kernel its image shipped with. The `Automation_Custom_PreScript.sh`
> you copy in step 2 is what stops DietPi's first-boot `apt upgrade` from bumping
> the kernel out from under it. Skip that file and your first `make deploy` will
> abort. See [DEPLOY.md](DEPLOY.md#kernel-pinning-on-a-freshly-flashed-card).

---

## 1. Flash the image

**With Raspberry Pi Imager:** choose "Use custom" and select the `.img.xz` file,
pick the card, write. Decline the customisation prompt.

**With `dd`** (macOS — replace `diskN` with your card, and check it twice with
`diskutil list`; writing to the wrong disk destroys it):

```sh
xz -dc DietPi_RPi234-ARMv8-Trixie.img.xz | \
  sudo dd of=/dev/rdiskN bs=4m status=progress
sync
```

On Linux the device is `/dev/sdX` and the flag is `bs=4M`.

Re-insert the card afterwards. The FAT boot partition mounts as
**`/Volumes/bootfs`** on macOS, or `/media/<user>/bootfs` on most Linux desktops.
The examples below use `$BOOT`:

```sh
BOOT=/Volumes/bootfs      # adjust for your platform
```

## 2. Copy the shared files

These two are identical for both roles and belong on **every** card:

```sh
cp docs/dietpi_config/Automation_Custom_PreScript.sh "$BOOT"/
cp docs/dietpi_config/Automation_Custom_Script.sh    "$BOOT"/
cp docs/dietpi_config/dietpi-wifi.txt                "$BOOT"/
```

| File | What it does |
|------|--------------|
| `Automation_Custom_PreScript.sh` | Runs **before** DietPi's first-run apt upgrade. Holds and APT-pins the kernel and firmware packages. **This is the one that actually prevents the kernel bump.** |
| `Automation_Custom_Script.sh` | Runs **after** first-run setup. Installs `avahi-daemon` (so `.local` names resolve and deploy can reach the Pi), masks Bluetooth and the APT daily timers, and removes DietPi's blacklists on the camera/codec/VC-shared-memory modules. |
| `dietpi-wifi.txt` | WiFi credentials, applied at first boot. |

## 3. Copy the role-specific files

Pick **one** role per card. Each provides three files that replace what the
image ships.

**TX card (camera side):**

```sh
cp docs/dietpi_config/tx/dietpi.txt  "$BOOT"/
cp docs/dietpi_config/tx/config.txt  "$BOOT"/
cp docs/dietpi_config/tx/cmdline.txt "$BOOT"/
```

**RX card (receiver side):**

```sh
cp docs/dietpi_config/rx/dietpi.txt  "$BOOT"/
cp docs/dietpi_config/rx/config.txt  "$BOOT"/
cp docs/dietpi_config/rx/cmdline.txt "$BOOT"/
```

What differs between the two roles:

| | TX | RX |
|---|---|---|
| Hostname | `pivid-tx` | `pivid-rx` |
| Headless | yes (`AUTO_SETUP_HEADLESS=1`), no framebuffers | no — a display is attached |
| Camera | `dtoverlay=imx290,clock-frequency=37125000`, `camera_auto_detect=0` | none |
| Display driver | none — libcamera reaches the ISP directly | `dtoverlay=vc4-kms-v3d,cma-128` |
| CMA reservation | `cma=128M` on the kernel command line | via the `vc4-kms-v3d` overlay |

Both roles share `dtparam=spi=on` (the LR2021 sits on SPI0),
`dtoverlay=disable-bt` (frees GPIO14/15 PL011 for the flight controller), and
`gpu_mem_512=76`.

> `gpu_mem_512=76` is **not** cosmetic. Anything lower makes the firmware loader
> pick `start_cd.elf`, the cut-down VPU firmware that omits the camera and codec
> services. The symptom on TX is libcamera reporting "No cameras available!" even
> with the ribbon attached; on RX it is `vc_sm_cma_vchi_init: failed to open VCHI
> service (-22)` and no `v4l2h264dec`.

## 4. Fill in your own credentials

The files in this repo ship with placeholders. **Both cards need this.**

**WiFi** — edit `$BOOT/dietpi-wifi.txt`:

```sh
aWIFI_SSID[0]='YOUR_WIFI_SSID'
aWIFI_KEY[0]='YOUR_WIFI_PASSPHRASE'
aWIFI_KEYMGR[0]='WPA-PSK'
```

For an open network set `aWIFI_KEY[0]=''` and `aWIFI_KEYMGR[0]='NONE'`. If your
passphrase contains a single quote, write it as `'\''`.

**SSH key** — edit `$BOOT/dietpi.txt` and paste **your own** public key:

```sh
AUTO_SETUP_SSH_PUBKEY=ssh-ed25519 AAAA... you@yourhost
```

Whatever key you put here gets passwordless root on the resulting image. Leave it
blank and you will not be able to deploy, since `deploy.sh` needs key-based root
ssh with no password prompt.

**Also check in `dietpi.txt`:**

| Setting | Default here | Note |
|---------|--------------|------|
| `AUTO_SETUP_NET_WIFI_COUNTRY_CODE` | `UA` | Set to your own country, or WiFi may pick illegal channels |
| `AUTO_SETUP_GLOBAL_PASSWORD` | `dietpi` | Change it; this is the root password |
| `AUTO_SETUP_NET_HOSTNAME` | `pivid-tx` / `pivid-rx` | Only change if you also change `TX_HOST`/`RX_HOST` at deploy time |

`AUTO_SETUP_APT_INSTALLS` is deliberately empty — runtime packages are installed
by `deploy.sh`, not at first boot. [DEPLOY.md](DEPLOY.md#package-install-model)
explains why.

## 5. First boot

Eject the card cleanly, insert it, and power the Pi. First boot runs DietPi's
automated setup: it joins WiFi, applies `dietpi.txt`, runs both custom scripts,
and reboots once or twice on its own. Allow **10–20 minutes** on a Zero 2 W, with
no input needed.

Verify from your workstation:

```sh
ping -c1 pivid-tx.local
ssh root@pivid-tx uname -r
```

If `.local` does not resolve, wait longer — `avahi-daemon` installs late in the
sequence. Failing that, find the Pi's address on your router and ssh by IP.

Confirm the kernel pin took, which is the whole point of the pre-script:

```sh
ssh root@pivid-tx 'apt-mark showhold'
# expect the linux-image-* / linux-headers-* / raspi-firmware / rpi-eeprom lines

ssh root@pivid-tx 'cat /var/lib/dietpi/logs/dietpi-automation_custom_prescript.log'
# expect: [pivid-prescript] held N kernel/firmware package(s)
```

Then repeat everything for the second card and role.

## 6. Next

Both Pis reachable by name, kernel held → continue with
[DEPLOY.md](DEPLOY.md). Note that the first install needs one extra reboot for
the device-tree overlay to instantiate `/dev/radio0`; `deploy.sh` tells you when.

## Troubleshooting

**First `make deploy` aborts with `FATAL: .ko built for '<X>' but <host> runs
'<Y>'`.** The kernel was upgraded before it got held — almost always a card
flashed without `Automation_Custom_PreScript.sh`. Recovery options are in
[DEPLOY.md](DEPLOY.md#kernel-pinning-on-a-freshly-flashed-card).

**The Pi never joins WiFi.** Check `dietpi-wifi.txt` is at the boot-partition
root and that `aWIFI_SSID[0]` is the `[0]` slot (DietPi tries slots in order).
The SSID is case-sensitive. Confirm the country code matches your region.

**"No cameras available!" on TX.** Check `gpu_mem_512=76` survived in
`$BOOT/config.txt`, and that `start_x=1` is **not** set — it enables the legacy
MMAL stack, which is mutually exclusive with libcamera.

**Custom scripts appear not to have run.** DietPi logs both:

```
/var/lib/dietpi/logs/dietpi-automation_custom_prescript.log
/var/lib/dietpi/logs/dietpi-automation_custom_script.log
```

A missing log means the file was not found at the boot-partition root — check
the exact filename, including case.
