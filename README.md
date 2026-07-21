# PiVid v2.1

A low-latency H.264 video link between two Raspberry Pi Zero 2 W boards over a
NiceRF LR2021 S-band radio running FLRC at 2.1 GHz.

The transmit Pi encodes camera video with `rpicam-vid` and fragments it onto the
radio. The receive Pi reassembles the stream and hands it to a caster, which
distributes it over the LAN to one or more desktop players; the receive Pi can
also drive a local HDMI or composite display directly.

Measured event-to-eyeball latency is **~150 ms median** (TX camera scene to RX
HDMI monitor, motion video at 950 kbps, filmed at 240 fps, n=132). About 35–40 ms
of that is the PiVid pipeline itself; the rest is camera and display physics. See
[docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

## Architecture

```
 +-------------+  H.264 bytes   +----------+  ring   +----------------+
 | rpicam-vid  | ──────────────▶│ pivid-tx │────────▶│ lr2021.ko (TX) │
 +-------------+   (stdin)      +----------+  mmap   +-------+--------+
                                                             │ SPI
                                              LR2021 radio (FLRC, 2.1 GHz)
                                                             │
                          ring   +----------+  pipe  +-----------+
 lr2021.ko (RX) ─────────────────▶│ pivid-rx │──────▶│pivid-caster│
 +--------------+           mmap   +----------+       +-----+------+
                                                            │ LAN (UDP video + telemetry)
                                                            ▼
                                                    +---------------+
                                                    | pivid-player  |  decode + OSD
                                                    | (Mac/Linux/Win)|  on the viewer
                                                    +---------------+
```

The repo holds one kernel driver, five userspace programs and a shared library:

| Path | Role |
|------|------|
| `drivers/lr2021/` | Thin SPI/IRQ proxy. Owns the SPI state machine, BUSY pin polling, threaded DIO9 interrupt, two mmap'd SPSC ring buffers, and a small ioctl surface. Exposes `/dev/radio0`. |
| `apps/pivid-tx/` | TX daemon. Reads Annex-B H.264 from stdin, fragments into 0–253-byte payloads with a 1-byte flags/SEQ header, pushes into the TX ring. Handles bind broadcast, bind LED and button. Carries the gated AQM frame-drop stage (`docs/aqm/`). |
| `apps/pivid-rx/` | RX daemon. Drains the RX ring, validates SEQ, reassembles NAL units, and emits the stream to `pivid-caster`, a UDP socket, or a local `gst-launch` sink. Bind-listen, bind LED, button. |
| `apps/pivid-caster/` | Distributes the RX stream over the LAN — accepts player subscriptions and fans out video plus telemetry to one or more viewers. |
| `apps/pivid-player/` | Cross-platform desktop viewer (macOS, Linux; Windows partial). Decodes H.264 in software by default and composites the MSP DisplayPort OSD over it. See `apps/pivid-player/README.md`. |
| `apps/pivid-config-agent/` | On-Pi config and web dashboard (civetweb). Reads and writes `pivid.conf`, applies changes via SIGHUP/modreload tiers, and serves the schema-driven settings UI plus a WPA2 config AP. Never opens `/dev/radio0`. |
| `common/` | Shared static library: protocol and packet helpers, canonical config parse/serialize, binding-phrase → sync-word derivation, apply-tier schema, libgpiod peripherals, ioctl UAPI, userspace ring accessors. |
| `tests/` | Host-side unit tests across `agent/ caster/ common/ driver/ integration/ player/ rx/ scripts/ tx/`, run in a `gcc:bookworm` container, plus `smoke_test.sh` for hardware verification over ssh. |
| `target/` | Files installed **on the Pi** — the path under `target/` is the path on the device (systemd units, `/opt/pivid` helpers, modprobe and modules-load config). |
| `scripts/` | Host-side tooling that never leaves the build machine: `deploy.sh`, the two patched-upstream cross-builds, and the web-asset embedder. |
| `hat/` | KiCad project for the carrier board (Pi Zero µHAT, 4-layer). |

## Hardware

LR2021 module on Pi SPI0:

| Pi GPIO | LR2021 pin | Direction | Notes |
|---------|------------|-----------|-------|
| MOSI/MISO/SCK/CE0 | MOSI/MISO/SCK/NSS | bidirectional | Standard SPI bus |
| GPIO17 | NRESET | output, active-low | Driver pulses to hard-reset |
| GPIO22 | BUSY | input | Driver polls before each SPI frame |
| GPIO27 | DIO9 | input + IRQ | Rising edge triggers the driver IRQ thread |

Peripherals (full pin-by-pin table in `docs/hardware/gpio.txt`):

| GPIO | Purpose |
|------|---------|
| GPIO5 | TX status LED — blinks while data is moving |
| GPIO6 | Bind LED — solid when bound, slow blink during bind, fast blink during new-bind |
| GPIO13 | Bind button — long-press starts the bind procedure |

The TX side can additionally drive an external SZM-2166Y power amplifier
(~36 dB gain, rated 33 dBm / 2 W, 2.3–2.7 GHz) from a 6S pack. The radio's own
output is set to −3 dBm so the amplifier runs at its rated output without input
overdrive. Note the default carrier (2.1 GHz) is **below** that amplifier's rated
band — see [docs/DECISIONS.md](docs/DECISIONS.md) before transmitting with it.

Both Pis are wired identically; the only configuration difference is which side
runs `pivid-tx.service` and which runs `pivid-rx.service`.

## Build, deploy, verify

```sh
make all     # build everything in Docker (tx, rx, caster, config-agent, player, lr2021.ko, lr2021.dtbo)
make tests   # host-side unit tests in a gcc:bookworm container
make deploy  # copy, install and enable services on both Pis (env: TX_HOST, RX_HOST)
make smoke   # hardware verification over ssh against the deployed Pis
```

Starting from bare hardware, prepare the two SD cards first —
[docs/SD_CARD.md](docs/SD_CARD.md) covers flashing the DietPi image and applying
the per-role boot config.

The Pis have no native compiler — everything cross-builds on the host or in
Docker into `bin/`, and `deploy.sh` copies the ARM binaries over. A first install
needs one reboot to pick up the device-tree overlay; see
[docs/DEPLOY.md](docs/DEPLOY.md).

## Documentation

- [docs/SD_CARD.md](docs/SD_CARD.md) — flashing the DietPi image and applying the per-role boot config
- [docs/BUILD.md](docs/BUILD.md) — Docker image, make targets, kernel pinning
- [docs/DEPLOY.md](docs/DEPLOY.md) — deploy, first-install reboot, smoke test, settings, bind, troubleshooting
- [docs/DECISIONS.md](docs/DECISIONS.md) — settled engineering decisions and the constraints behind them
- [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md) — measured results: latency budget, RF and SPI campaigns, FEC sweeps
- [docs/protocol/](docs/protocol/) — wire format, packet layout, FEC, reassembly, OSD grid, telemetry, LAN chunking
- [docs/aqm/README.md](docs/aqm/README.md) — feeder-level frame-drop AQM
- [docs/MSP_DISPLAYPORT_OSD.md](docs/MSP_DISPLAYPORT_OSD.md) — MSP DisplayPort OSD relay
- [docs/hardware/gpio.txt](docs/hardware/gpio.txt) — full pin-by-pin wiring table
- [docs/hidden-env-knobs.md](docs/hidden-env-knobs.md) — environment variables not exposed in the UI
- [TODO.md](TODO.md) — open work

## Licence

Copyright (C) 2026 Pi.

- **Software** — GPL-3.0-or-later, plus a section 7(b) term requiring that the
  PiVid attribution be preserved in user-visible notices.
- **Kernel code** (`drivers/lr2021/`, the `bcm2835-codec` patches, and the tests
  that include driver headers) — GPL-2.0-only, as Linux derivative works.
- **Hardware** (`hat/`) — CERN-OHL-P-2.0. Fork it, modify it, sell boards; just
  keep the notices.

You can sell hardware and products built on PiVid. You cannot ship the software
in closed form, and you cannot call your fork "PiVid". See [LICENSE](LICENSE) for
the authoritative per-path map, and a note for distributors about FFmpeg builds.
