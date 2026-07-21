# pivid-player

Cross-platform desktop viewer for the PiVid stream: software-decoded H.264 over
raw UDP with a Cairo OSD composited over it on the GPU via SDL2.

## Build (macOS, the verified path)

    brew install sdl2 ffmpeg cairo pkg-config
    cd apps/pivid-player
    cmake -B build -S .
    cmake --build build

## OSD font

The OSD renders an MSP DisplayPort character grid using a **compiled-in**
BTFL_DJI msp-osd bitmap font (the PNG sheet is embedded at build time by
`EmbedFile.cmake`). There is no font-file argument — nothing to supply at
runtime.

## Run

With no `--caster`, the player **auto-discovers** the caster over mDNS (browses
`_pivid._udp` and subscribes to the first one found, showing "Searching for
caster…" until it resolves):

    ./build/pivid-player

To target a caster explicitly (skips discovery — also the override for multi-homed
hosts where the query may exit the wrong interface):

    ./build/pivid-player --caster <rx-pi-ip>

Options: `--caster <ip>` (default: auto-discover via mDNS), `--port <p>` (caster
control port; default: from mDNS, else 5700), `--width`/`--height` (default: size
to the first decoded frame), `--hw` (hardware decode; see below), `--version`.
Run `--help` for the full list.

The player is configured **entirely by command-line switches** — there is no
config file. Run with no switches to auto-discover a caster via mDNS; pass
`--caster <ip>` to skip discovery.

The player's own video + telemetry **receive ports are auto-assigned** (it binds
ephemeral ports) and advertised to the caster over the control channel — there is
nothing to configure. The caster's control port is the only externally-meaningful
port: it comes from the mDNS SRV record, or `--port` under an explicit `--caster`,
else the 5700 default.

The OSD status banner uses an OS-appropriate monospace font picked automatically
(macOS: Menlo, Windows: Consolas, Linux: fontconfig's generic `monospace`, which
resolves even on headless installs).

## Run on the RX Pi (aarch64, headless HDMI)

The player cross-compiles for the RX Pi to drive a **local HDMI display** (no
Mac/PC viewer needed). It runs as a systemd unit (`pivid-player.service`) gated
by the dashboard **Local HDMI player** toggle (config key `PLAYER_LOCAL`):

- **On**  → `systemctl enable --now pivid-player` (starts now and at every boot).
- **Off** → `systemctl disable --now pivid-player` (stops it and keeps it off).

`deploy.sh` provisions everything the RX needs to run it: the runtime libs
(`libsdl2-2.0-0 libavcodec-extra libcairo2`, via the RX package set), the KMS
display overlay (`dtoverlay=vc4-kms-v3d` — **reboot once** after first install;
the appliance image's `config.txt` already ships it), and the service unit
(installed **disabled**; the toggle owns enable/disable).

**Build (on the dev host, via Docker):**

    make player                 # -> bin/pivid-player (aarch64 ELF)

then `./scripts/deploy.sh`. The toggle stays a no-op until the binary is
present — `op_player_set` refuses to enable the unit when `/opt/pivid/pivid-player`
is missing, and deploy prints a reminder if you flip it on too early.

The unit launches:

    SDL_VIDEODRIVER=kmsdrm /opt/pivid/pivid-player --caster 127.0.0.1 --width 640 --height 480 --fullscreen

The explicit `--width/--height` match the 640×480 prod source so the player
switches the CRTC to the closest (smaller) panel mode rather than scanning out at
native 1080p — cutting memory-bus bandwidth that would otherwise contend with the
radio's SPI-DMA read on the Pi Zero 2 W.

Software decode (the default) conceals on the lossy link; `--hw` is NOT used on
the Pi (the VideoCore is a V4L2 M2M codec, not VAAPI) and would black out on the
live stream. To verify the SDL2 KMSDRM backend is present after a build:

    SDL_VIDEODRIVER=kmsdrm /opt/pivid/pivid-player --help

(it should not error with "kmsdrm not available"; `ldd /opt/pivid/pivid-player |
grep -E 'libdrm|gbm'` should show the DRM/GBM libs SDL2 pulls in.)

## Decode mode (default: software)

The player decodes in **software by default**. On a damaged picture the platform
HW decoders (VideoToolbox on macOS, D3D11VA/VAAPI elsewhere) tear down and rebuild
their decode session ("VideoToolbox decoder needs reconfig, restarting..") and
then emit nothing until the next IDR (~1 s). The live RF stream uses
FEC-unprotected droppable P-frames, so frames are damaged far more often than that
-- HW decode ends up in a perpetual teardown/reacquire cycle (black window). The
software decoder conceals each damaged frame in place and stays locked; at 480p30
it is cheap. (Measured on a real live capture: software 361/361 frames, 0 errors;
VideoToolbox restarts its session on the same bytes.)

- `--hw` -- opt into the platform HW decoder. Only safe on a *clean* (lossless)
  stream, e.g. a local test pattern; it will black out on the live link.
- Software decode is the default; there is no `--sw` switch (just omit `--hw`).

The window starts black with the OSD drawn from telemetry (which usually arrives
before the first video keyframe); video appears at the next TX IDR.

## Cross-check

`ffplay -fflags nobuffer -flags low_delay -i udp://@:5000` views the raw stream
(no OSD) and confirms the caster is forwarding video to this host.

## Other platforms (wired, unverified in v1)

Linux: `apt install libsdl2-dev libav*-dev libcairo2-dev`. Windows: vcpkg
`sdl2 ffmpeg cairo`. Software decode is the default everywhere; `--hw`
opts into VAAPI / D3D11VA respectively (clean streams only). Only macOS is verified.
