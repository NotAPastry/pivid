# PiVid v2.1 top-level Makefile.
#
# Everything builds inside the `pivid-builder` Docker image, which carries the
# pinned RPi kernel tree at /opt/rpi-linux. The host needs only Docker and a
# POSIX shell -- no toolchain on the host.
#
# Artifacts produced under bin/:
#   - pivid-tx, pivid-rx   (stripped aarch64 userspace binaries)
#   - lr2021.ko            (stripped kernel module, --strip-debug)
#   - lr2021.dtbo          (device-tree overlay for spi0.0)

.PHONY: all userspace driver overlay codec rpicam tx latprobe-tx rx latprobe-rx agent caster latprobe-caster player tests deploy smoke clean docker-build docker-build-rpicam docker-build-tests flrc-probe-driver coalesce-probe-driver txperf-driver

ROOT := $(CURDIR)

# Short git hash stamped into every binary at build time so a running unit can
# report exactly which source it was built from (surfaced in the agent
# dashboard's System panel; logged at daemon startup). "-dirty" is appended when
# the working tree has uncommitted changes. Computed on the host (the builder
# container has no git) and passed into the container via -e.
GIT_HASH := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet 2>/dev/null || echo -dirty)
DRUN := docker run --rm \
            -v $(ROOT):/work \
            -w /work \
            -e BUILD_VERSION=$(GIT_HASH) \
            pivid-builder

# Image for host-side unit tests (Phase 1 libraries). Separate from
# pivid-builder so unit tests don't depend on the slower kernel-tree image.
# Built from docker/Dockerfile.tests (gcc:bookworm + libcairo2-dev + pkg-config
# + fonts-dejavu-core) so the cairo-backed OSD renderer test can link/render.
TEST_IMAGE := pivid-tests
TEST_DRUN  := docker run --rm \
                  -v $(ROOT):/work \
                  -w /work \
                  $(TEST_IMAGE)

all: userspace driver overlay codec rpicam

# Build all four daemons in a SINGLE builder container (one `docker run`, not
# four) -- the container cold-start dominates the incremental compile. The
# individual tx/rx/agent/caster targets below remain for standalone builds.
userspace: | docker-build
	$(DRUN) sh -c '\
	    make -C apps/pivid-tx \
	 && make -C apps/pivid-rx \
	 && make -C apps/pivid-config-agent \
	 && make -C apps/pivid-caster'

docker-build:
	docker build -f docker/Dockerfile.rpi64 -t pivid-builder .

# Order-only (`|`) prereq on the builder image: guarantees it exists before any
# compile and makes `make -jN` race-free, without the phony image timestamp
# forcing unnecessary rebuilds of file targets.
tx: | docker-build
	$(DRUN) make -C apps/pivid-tx

# Latency-budget probe build of pivid-tx (S1 parser + S2 FEC block-hold, gated
# -DLATPROBE). clean first so the -D change forces a full recompile (the %.o rule
# alone doesn't track -D flips); build LATPROBE; copy the probe binary to
# bin/pivid-tx.latprobe; then clean AGAIN so the tree is left with NO probe .o
# files and NO probe-contaminated bin/pivid-tx -- a later `make tx` builds a clean
# production binary. The trailing clean removes $(OBJS)/$(DEPS)/bin/pivid-tx but
# leaves bin/pivid-tx.latprobe, the sole artifact of this target.
latprobe-tx: | docker-build
	$(DRUN) sh -c 'make -C apps/pivid-tx clean && make -C apps/pivid-tx LATPROBE=1 && cp bin/pivid-tx bin/pivid-tx.latprobe && make -C apps/pivid-tx clean && file bin/pivid-tx.latprobe'

rx: | docker-build
	$(DRUN) make -C apps/pivid-rx

# Latency-budget probe build of pivid-rx (S5 FEC-close + S6a RX-forward, gated
# -DLATPROBE). Same hygiene as latprobe-tx: clean first so the -D change forces a
# full recompile (the %.o rule alone doesn't track -D flips); build LATPROBE; copy
# the probe binary to bin/pivid-rx.latprobe; then clean AGAIN so the tree is left
# with NO probe .o files and NO probe-contaminated bin/pivid-rx -- a later `make
# rx` builds a clean production binary. bin/pivid-rx.latprobe is the sole artifact.
latprobe-rx: | docker-build
	$(DRUN) sh -c 'make -C apps/pivid-rx clean && make -C apps/pivid-rx LATPROBE=1 && cp bin/pivid-rx bin/pivid-rx.latprobe && make -C apps/pivid-rx clean && file bin/pivid-rx.latprobe'

agent: | docker-build
	$(DRUN) make -C apps/pivid-config-agent

caster: | docker-build
	$(DRUN) make -C apps/pivid-caster

# Latency-budget probe build of pivid-caster (S6b caster-forward, gated
# -DLATPROBE). Same hygiene as latprobe-tx/latprobe-rx: clean first so the -D
# change forces a full recompile (the %.o rule alone doesn't track -D flips);
# build LATPROBE; copy the probe binary to bin/pivid-caster.latprobe; then clean
# AGAIN so the tree is left with NO probe .o files and NO probe-contaminated
# bin/pivid-caster -- a later `make caster` builds a clean production binary.
# bin/pivid-caster.latprobe is the sole artifact.
latprobe-caster: | docker-build
	$(DRUN) sh -c 'make -C apps/pivid-caster clean && make -C apps/pivid-caster LATPROBE=1 && cp bin/pivid-caster bin/pivid-caster.latprobe && make -C apps/pivid-caster clean && file bin/pivid-caster.latprobe'

# pivid-player for the RX Pi (aarch64). NOT part of `all`: a normal radio
# deploy must not require the SDL2/FFmpeg player build. Cross-builds in the
# pivid-builder image via the CMake toolchain file, strips, and drops the ELF
# in bin/. Runtime apt deps (libsdl2-2.0-0, libavcodec/format/util, libcairo2)
# + dtoverlay=vc4-kms-v3d are documented manual prereqs on the RX Pi.
# pivid-player for the local Mac (native build, no docker). Platform-named
# build dir mirrors build-arm: apps/pivid-player/build-macos.
player-macos:
	cmake -S apps/pivid-player -B apps/pivid-player/build-macos -DCMAKE_BUILD_TYPE=Release
	cmake --build apps/pivid-player/build-macos -j
	@echo "-> apps/pivid-player/build-macos/pivid-player"

player: | docker-build
	$(DRUN) sh -c '\
	    cmake -S apps/pivid-player -B apps/pivid-player/build-arm \
	          -DCMAKE_TOOLCHAIN_FILE=/work/apps/pivid-player/cmake/aarch64-linux.cmake \
	          -DCMAKE_BUILD_TYPE=Release \
	 && cmake --build apps/pivid-player/build-arm -j \
	 && aarch64-linux-gnu-strip apps/pivid-player/build-arm/pivid-player \
	 && mkdir -p bin \
	 && cp apps/pivid-player/build-arm/pivid-player bin/pivid-player'

# Driver:
#   1. cross-compile out-of-tree against /opt/rpi-linux. The image carries
#      both the prepared kernel source tree AND Module.symvers from the
#      matching headers .deb -- the latter is what gives our .ko's
#      __versions section real symbol CRCs so it loads on a
#      CONFIG_MODVERSIONS=y kernel. (Without it, modpost emits zero CRCs and
#      the runtime check rejects with "no symbol version for module_layout".)
#   2. strip debug symbols (--strip-debug keeps .symtab/.strtab needed by the
#      module loader; removes function/variable names useful for RE)
#   3. copy .ko to bin/
#
# Kbuild scatters ~17 intermediate files (.o, .mod, .mod.c, .ko, .cmd, ...)
# into drivers/lr2021/ during the build. We DO NOT clean them up afterwards:
# .gitignore masks all of them (`*.o`, `*.ko`, `*.mod`, `*.mod.c`, `*.cmd`,
# `*.symvers`, `*.order`), and keeping them in place enables Kbuild's
# incremental rebuild (next `make driver` reuses unchanged .o files).
# `make clean` (at the project root) removes them when wanted.
driver: | docker-build
	$(DRUN) sh -c '\
	    make -C drivers/lr2021 modules \
	 && aarch64-linux-gnu-strip --strip-debug drivers/lr2021/lr2021.ko \
	 && mkdir -p bin \
	 && cp drivers/lr2021/lr2021.ko bin/lr2021.ko \
	 && file bin/lr2021.ko'

# Patched VPU codec module -- ONE module that EXPOSES the resilience controls
# (multi-slice, intra-refresh, droppable-P, bitrate-mode) with neutral/stock
# defaults. Recipe selection is done at encode time by the patched rpicam-vid's
# CLI flags (--bitrate-mode/--intra-refresh/--slice-mb/--droppable-*), built
# from common/camera_argv.c. No live .ko swap.
codec: | docker-build
	PIVID_PATCH=patches/bcm2835-codec-controls.patch bash scripts/build-bcm2835-codec.sh

docker-build-rpicam:
	docker build -f docker/Dockerfile.rpicam -t pivid-rpicam-builder:latest .

# Test image: gcc:bookworm + cairo/pkg-config/DejaVu fonts for the host-side
# unit tests (the OSD renderer test links cairo and renders into a surface).
docker-build-tests:
	docker build -f docker/Dockerfile.tests -t $(TEST_IMAGE) .

# Patched rpicam-vid + librpicam_app.so (CLI-flag encoder controls).
rpicam: | docker-build-rpicam
	bash scripts/build-rpicam-apps.sh

# Device-tree overlay. dtc is baked into the builder image.
overlay: bin/lr2021.dtbo

bin/lr2021.dtbo: drivers/lr2021/lr2021-overlay.dts | docker-build
	mkdir -p bin
	$(DRUN) dtc -@ -I dts -O dtb \
	            -o bin/lr2021.dtbo \
	            drivers/lr2021/lr2021-overlay.dts

# Host-side unit tests, run inside a Linux x86_64 container (so the host
# can be macOS without a local toolchain).
tests: docker-build-tests
	$(TEST_DRUN) make -C tests run

deploy: all
	bash scripts/deploy.sh

# Hardware smoke test. Read-only ssh checks against both Pis;
# requires `make deploy` has run. Override TX_HOST/RX_HOST/DEPLOY_USER on
# the command line or in the environment.
smoke:
	bash tests/smoke_test.sh all

clean:
	$(DRUN) make -C drivers/lr2021 clean   2>/dev/null || true
	$(DRUN) make -C apps/pivid-tx   clean  2>/dev/null || true
	$(DRUN) make -C apps/pivid-rx   clean  2>/dev/null || true
	$(DRUN) make -C apps/pivid-config-agent clean 2>/dev/null || true
	$(DRUN) make -C apps/pivid-caster clean 2>/dev/null || true
	$(TEST_DRUN) make -C tests clean       2>/dev/null || true
	rm -rf bin/

# Debug/test ONLY (FLRC_PROBE): driver build that dmesg-logs per-packet FLRC
# framing (rx pkt_len/crc, dyn_tx lengths, LenError FIFO level). Self-contained
# -- it needs no userspace helper. Clean first so the -D define reaches every
# object; restore production with `make clean && make driver`.
flrc-probe-driver: | docker-build
	$(DRUN) sh -c '\
	    make -C drivers/lr2021 clean && \
	    make -C drivers/lr2021 modules KCFLAGS=-DFLRC_PROBE && \
	    aarch64-linux-gnu-strip --strip-debug drivers/lr2021/lr2021.ko && \
	    mkdir -p bin && cp drivers/lr2021/lr2021.ko bin/lr2021.ko && \
	    file bin/lr2021.ko'

# Debug/test ONLY (COALESCE_PROBE): multi-packet-per-IRQ coalescing probe.
# Samples GetRxFifoLevel at every RX_DONE and dmesg-logs packets whose FIFO
# holds >= 6 B beyond this packet's remaining bytes (a second packet queued
# behind: the GetRxPktLength-describes-the-LAST-payload hazard, DS 7.3.10).
# Clean first so the -D define reaches every object; restore production with
# `make clean && make driver`.
coalesce-probe-driver: | docker-build
	$(DRUN) sh -c '\
	    make -C drivers/lr2021 clean && \
	    make -C drivers/lr2021 modules KCFLAGS=-DCOALESCE_PROBE && \
	    aarch64-linux-gnu-strip --strip-debug drivers/lr2021/lr2021.ko && \
	    mkdir -p bin && cp drivers/lr2021/lr2021.ko bin/lr2021.ko && \
	    file bin/lr2021.ko'

# Debug-only TX profiling .ko (TXPERF): submit_locked dumps avg(us)
# submit/air/gap/period every 2000 packets via dev_info (TXPERF dmesg line).
# Clean first so the -DTXPERF define reaches every object. Restore
# production with `make clean && make driver`.
txperf-driver: | docker-build
	$(DRUN) sh -c '\
	    make -C drivers/lr2021 clean && \
	    make -C drivers/lr2021 modules KCFLAGS=-DTXPERF && \
	    aarch64-linux-gnu-strip --strip-debug drivers/lr2021/lr2021.ko && \
	    mkdir -p bin && cp drivers/lr2021/lr2021.ko bin/lr2021.ko && \
	    file bin/lr2021.ko'
