# Building PiVid v2.1

Everything cross-compiles inside the `pivid-builder` Docker image. The
build host needs only Docker; no aarch64 toolchain, no kernel headers,
no `dtc`. Works on macOS and Linux.

## Prerequisites

- Docker (Desktop on macOS, engine on Linux). The first image build is
  ~5 minutes because it clones the RPi kernel tree and runs
  `modules_prepare` once; subsequent builds reuse the layer cache.
- About 4 GB of disk for the builder image (most of it is the RPi
  kernel source tree at `/opt/rpi-linux` inside the image).
- That's it. No host toolchain.

## Quick reference

```sh
make all          # docker-build + tx + rx + driver + overlay
make tx           # only apps/pivid-tx → bin/pivid-tx
make rx           # only apps/pivid-rx → bin/pivid-rx
make driver       # only drivers/lr2021 → bin/lr2021.ko
make overlay      # only the DT overlay → bin/lr2021.dtbo
make tests        # host-side unit tests (gcc:bookworm container)
make clean        # nuke bin/ and Kbuild intermediates
```

## First build

```sh
make docker-build   # implicit dependency of every other target
```

This builds the `pivid-builder` image from `docker/Dockerfile.rpi64`.
The image:

1. Pulls Debian trixie (must match the target Pi's distro — trixie
   dropped `libgpiod2` and only ships `libgpiod3` (the v2 API).
   Building on bookworm produces binaries that link `libgpiod.so.2`,
   which has no apt-installable provider on the target),
2. installs the host toolchain (`gcc`), aarch64 cross-toolchain
   (`gcc-aarch64-linux-gnu`, `binutils-aarch64-linux-gnu`,
   `libc6-dev-arm64-cross`), libgpiod headers for arm64
   (`libgpiod-dev:arm64` → libgpiod v2), the kernel build dependencies
   (`bc bison flex libssl-dev libelf-dev`), and `device-tree-compiler`,
3. fetches `github.com/raspberrypi/linux` at the pinned commit
   `RPI_KERNEL_GIT_REF` (currently
   `66318c5b5cd8145a662303af12ed39ac07eb27cc`, the rpi-6.18.y merge
   dated 2026-05-11) into `/opt/rpi-linux`,
4. copies `docker/rpi-kernel.config` to `.config` in the kernel tree
   (the byte-exact `/boot/config-<uname-r>` pulled from a running Pi),
5. drops a `localversion-rpt` file containing `+rpt-rpi` into the
   source root so `scripts/setlocalversion` reproduces the Debian
   package's vermagic suffix,
6. runs `make olddefconfig && make modules_prepare` so the kernel tree
   is ready for out-of-tree module builds,
7. strips the kernel `.o` and `.o.cmd` artifacts to keep the image
   small (we only need the headers, scripts, and `.config`).

The Dockerfile also sets `ENV LOCALVERSION=` (empty), which suppresses
the trailing `+` that `setlocalversion --short` would otherwise emit
for a non-tagged HEAD. With these three pinned inputs (commit,
`.config`, `localversion-rpt`) plus that env var, the kernel tree
produces `KERNELRELEASE = 6.18.29+rpt-rpi-v8` -- byte-identical to the
Pi.

After that, every `make` target is a thin `docker run` wrapper that
mounts the repo at `/work` and invokes the right per-project Makefile.

## Output layout

```
bin/
├── pivid-tx          aarch64 ELF, stripped
├── pivid-rx          aarch64 ELF, stripped (links libgpiod.so.3 + libc.so.6)
├── lr2021.ko         aarch64 kernel module, --strip-debug (symtab kept for loader)
└── lr2021.dtbo       device-tree overlay binary (945 B)
```

The userspace binaries are stripped with the default `strip`. The
kernel module is stripped with `--strip-debug` so `.symtab` /
`.strtab` survive — the in-kernel module loader needs them. Source
never ships to the Pi.

## Kernel pinning model

The build is locked to a single kernel version per `pivid-builder`
image. Three coordinated inputs reproduce the running kernel's vermagic
byte-for-byte:

| Input | Lives in | Contributes |
|---|---|---|
| `RPI_KERNEL_GIT_REF` (build ARG) | `docker/Dockerfile.rpi64` | the kernel source tree |
| `docker/rpi-kernel.config` | repo root | `CONFIG_LOCALVERSION` + every feature flag |
| `localversion-rpt` | injected at image-build time | the `+rpt-rpi` suffix |

Linux enforces a vermagic check at `modprobe` time — the precompiled
`.ko` only loads against a running kernel whose vermagic matches.
`scripts/deploy.sh` pre-checks this:

```sh
KO_VERMAGIC="$(grep -ao 'vermagic=[^[:cntrl:]]*' bin/lr2021.ko | head -1 | sed 's/^vermagic=//' | awk '{print $1}')"
# Compares against `ssh root@pi uname -r`; aborts if they differ.
```

To **bump the kernel** (after updating a Pi's OS, or re-pointing the
build at a different Pi image):

1. `ssh root@<pi> uname -r` — note the running kernel release, e.g.
   `6.18.29+rpt-rpi-v8`.
2. `scp root@<pi>:/boot/config-<uname-r> docker/rpi-kernel.config` —
   capture the exact `.config` Debian used to build that kernel.
3. Find the matching upstream commit. The Debian changelog gives it:
   ```sh
   ssh root@<pi> zcat /usr/share/doc/linux-image-<uname-r>/changelog.Debian.gz | head -20
   ```
   Look for the `Linux commit: <40-char SHA>` line. Sanity-check the
   SHA exists in `raspberrypi/linux`:
   ```sh
   curl -s -o /dev/null -w "%{http_code}\n" \
       https://api.github.com/repos/raspberrypi/linux/commits/<SHA>
   ```
4. Edit `RPI_KERNEL_GIT_REF` in `docker/Dockerfile.rpi64`. Use the
   40-char SHA (strictly reproducible) or a release tag like
   `stable_20250916` (less reproducible — branches drift).
5. `docker build --no-cache -f docker/Dockerfile.rpi64 -t pivid-builder .`
   (~5 minutes; the kernel `modules_prepare` step dominates).
6. `make all && make deploy`.

If the Pi runs a non-Debian-packaged kernel (custom build, vanilla
upstream, etc.), the `+rpt-rpi` suffix in `localversion-rpt` will be
wrong. Adjust the Dockerfile's `printf '+rpt-rpi' > ...` line to
match the target's actual vermagic suffix — or remove it entirely
for a vanilla kernel.

There is **no DKMS** and no on-Pi rebuild path — that's by design (see
the design doc's "binary-only deploy" section).

## Host-side tests

`make tests` runs 20 binaries inside a `gcc:bookworm` container (the
`pivid-builder` image is not used — these tests have no aarch64 or
kernel dependency):

- `test_protocol`  — packet build/parse, byte-wise wire format
- `test_settings`  — `/opt/pivid/pivid.conf` round-trip + atomic write
- `test_phrase_sync` — SHA-256 KATs + binding-phrase → sync-word derivation
- `test_config_schema` — apply-tier metadata routing + drift guard
- `test_ring`      — kernel SPSC ring semantics (host fake)
- `test_ring_user` — userspace ring producer + consumer
- `test_encoder`   — H.264 chunker, paced-writer edge cases
- `test_reasm`     — NAL reassembly, 9 SEQ-gap / timeout / overflow cases
- `test_rx_loop`   — poll-driven main loop drain semantics

There are no on-target tests in this repo. Hardware integration is
covered by `tests/smoke_test.sh`, which runs on the host
workstation and ssh's into the deployed Pi's — see
[DEPLOY.md](DEPLOY.md).

## Cleaning

```sh
make clean   # removes bin/ + Kbuild intermediates inside drivers/lr2021/
```

The Kbuild intermediates (`.o`, `.mod`, `.mod.c`, `.cmd`, `.symvers`,
`.order`) are gitignored. Leaving them in place between builds enables
Kbuild's incremental rebuild — useful when iterating on driver C
files.
