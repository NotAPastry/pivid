# tests/Makefile -- host-architecture unit tests, grouped by subsystem subfolder.
#
# Invoked by the top-level Makefile via:
#   docker run --rm -v $ROOT:/work -w /work gcc:bookworm make -C tests run
#
# This Makefile (and `make`'s CWD) stays at tests/, so every ../common, ../apps,
# ../drivers, ../tools source path and every -I flag is CWD-relative and works
# regardless of which subfolder a test_*.c lives in. Only the target name and the
# test's own .c path carry a subfolder prefix.

CC := gcc
# -I../common reaches radio_ioctl.h, which lr2021_ring.h includes by short name.
CFLAGS := -Wall -Wextra -Wpedantic -std=c11 -I.. -I../common -I.

# Grouped by owning subsystem. Cross-app tests with no single owner -> integration/.
TEST_TARGETS := \
  common/test_protocol common/test_protocol_type common/test_telemetry \
  common/test_pkt_hist common/test_resid_hist common/test_settings common/test_phrase_sync \
  common/test_config_schema common/test_field_str common/test_settings_validate \
  common/test_settings_diff \
  common/test_cli_args common/test_fec common/test_camera_argv \
  common/test_stats common/test_kbps common/test_link_capacity common/test_lanfrag \
  common/test_ring_user common/test_tx_push_mt common/test_osd_grid \
  tx/test_encoder tx/test_fec_tx tx/test_sps_reinject tx/test_msp_parse \
  tx/test_msp_request tx/test_aqm tx/test_tx_csv \
  tx/test_fec_class tx/test_dp_client tx/test_camera_spawn \
  rx/test_reasm rx/test_rx_loop rx/test_fec_rx rx/test_osd_grid_rx \
  driver/test_ring driver/test_frame_sync driver/test_rx_walk driver/test_flrc_cfg \
  driver/test_rf_cfg driver/test_status \
  agent/test_sysstat agent/test_statusfile agent/test_form_parse \
  agent/test_modprobe_gen agent/test_apply_engine agent/test_schema_json \
  player/test_player_args player/test_player_state \
  player/test_mdns_parse \
  player/test_osd_glyph_cache \
  player/test_osd_msp_font \
  player/test_lan_reasm \
  player/test_au_asm \
  player/test_lan_source_core \
  player/test_frame_gate \
  caster/test_caster_control caster/test_caster_subscribers caster/test_caster_egress \
  integration/test_fec_e2e integration/test_frame_sync_fec

# NOTE: rx/test_telem_tx_smoke and caster/test_caster_smoke are host-only manual
# smoke checks (sockets/threads/timing, bind fixed ports) -- deliberately NOT in
# TEST_TARGETS, same as before the reorg. Build/run them by hand when needed.

.PHONY: all run clean

all: $(TEST_TARGETS)

# ---- common/ -------------------------------------------------------------
common/test_lanfrag: common/test_lanfrag.c ../common/lanfrag.h ../common/protocol.h
	$(CC) $(CFLAGS) -o $@ common/test_lanfrag.c

common/test_protocol: common/test_protocol.c ../common/protocol.c
	$(CC) $(CFLAGS) -o $@ $^

common/test_protocol_type: common/test_protocol_type.c ../common/protocol.c
	$(CC) $(CFLAGS) -o $@ $^

common/test_telemetry: common/test_telemetry.c ../common/protocol.c
	$(CC) $(CFLAGS) -o $@ $^

common/test_pkt_hist: common/test_pkt_hist.c ../common/pkt_hist.c ../common/pkt_hist.h
	$(CC) $(CFLAGS) -I../common -o $@ common/test_pkt_hist.c ../common/pkt_hist.c

common/test_resid_hist: common/test_resid_hist.c ../common/resid_hist.c ../common/resid_hist.h
	$(CC) $(CFLAGS) -I../common -o $@ common/test_resid_hist.c ../common/resid_hist.c

common/test_settings: common/test_settings.c ../common/settings.c ../common/phrase_sync.c ../common/sha256.c ../common/config_schema.c ../common/recipe_defaults.c
	$(CC) $(CFLAGS) -o $@ $^

common/test_phrase_sync: common/test_phrase_sync.c ../common/phrase_sync.c ../common/sha256.c
	$(CC) $(CFLAGS) -o $@ $^

common/test_config_schema: common/test_config_schema.c ../common/config_schema.c ../common/config_schema.h ../common/recipe_defaults.c ../common/recipe_defaults.h
	$(CC) $(CFLAGS) -o $@ common/test_config_schema.c ../common/config_schema.c ../common/recipe_defaults.c

common/test_field_str: common/test_field_str.c ../common/settings.c ../common/phrase_sync.c ../common/sha256.c ../common/config_schema.c
	$(CC) $(CFLAGS) -o $@ $^

common/test_settings_validate: common/test_settings_validate.c ../common/settings.c ../common/phrase_sync.c ../common/sha256.c ../common/config_schema.c
	$(CC) $(CFLAGS) -o $@ $^

common/test_settings_diff: common/test_settings_diff.c ../common/settings.c ../common/phrase_sync.c ../common/sha256.c ../common/config_schema.c
	$(CC) $(CFLAGS) -o $@ $^

common/test_cli_args: common/test_cli_args.c ../common/cli_args.c ../common/cli_args.h
	$(CC) $(CFLAGS) -o $@ common/test_cli_args.c ../common/cli_args.c

common/test_camera_argv: common/test_camera_argv.c ../common/camera_argv.c ../common/camera_argv.h
	$(CC) $(CFLAGS) -o $@ common/test_camera_argv.c ../common/camera_argv.c

common/test_fec: common/test_fec.c ../common/fec.c ../common/fec.h
	$(CC) $(CFLAGS) -I../common -o $@ common/test_fec.c ../common/fec.c

common/test_stats: common/test_stats.c ../common/status_file.c ../common/status_file.h \
            ../common/radio_ioctl.h
	$(CC) $(CFLAGS) -I../common -o $@ common/test_stats.c ../common/status_file.c

common/test_kbps: common/test_kbps.c ../common/kbps.h
	$(CC) $(CFLAGS) -o $@ common/test_kbps.c

common/test_link_capacity: common/test_link_capacity.c ../common/link_capacity.c ../common/link_capacity.h
	$(CC) $(CFLAGS) -I../common -o $@ common/test_link_capacity.c ../common/link_capacity.c

common/test_ring_user: common/test_ring_user.c ../common/ring_user.c ../common/ring_user.h ../common/radio_ioctl.h
	$(CC) $(CFLAGS) -o $@ common/test_ring_user.c ../common/ring_user.c

# test_tx_push_mt drives the same mutex+ring_user_push pattern tx_push uses,
# with two concurrent producer threads and one consumer -- needs -lpthread.
common/test_tx_push_mt: common/test_tx_push_mt.c ../common/ring_user.c ../common/ring_user.h ../common/radio_ioctl.h
	$(CC) $(CFLAGS) -o $@ common/test_tx_push_mt.c ../common/ring_user.c -lpthread

# test_osd_grid: pure grid model + sparse-span keyframe codec + reassembly.
common/test_osd_grid: common/test_osd_grid.c ../common/osd_grid.c ../common/osd_grid.h
	$(CC) $(CFLAGS) -I../common -o $@ common/test_osd_grid.c ../common/osd_grid.c

# ---- tx/ -----------------------------------------------------------------
# test_encoder links against mock_tx.c (recording stubs) so the encoder
# can be exercised without ioctls/mmap. -pthread for the wake-up test.
tx/test_encoder: tx/test_encoder.c tx/mock_tx.c \
              ../apps/pivid-tx/src/encoder.c \
              ../apps/pivid-tx/src/fec_tx.c \
              ../apps/pivid-tx/src/sps_reinject.c \
              ../apps/pivid-tx/src/aqm.c \
              ../apps/pivid-tx/src/tx_csv.c \
              ../common/fec.c ../common/protocol.c ../common/pkt_hist.c \
              ../common/resid_hist.c
	$(CC) $(CFLAGS) -pthread -I../apps/pivid-tx/src -I../common -o $@ \
	    tx/test_encoder.c tx/mock_tx.c \
	    ../apps/pivid-tx/src/encoder.c ../apps/pivid-tx/src/fec_tx.c \
	    ../apps/pivid-tx/src/sps_reinject.c \
	    ../apps/pivid-tx/src/aqm.c ../apps/pivid-tx/src/tx_csv.c \
	    ../common/fec.c ../common/protocol.c ../common/pkt_hist.c \
	    ../common/resid_hist.c

tx/test_fec_tx: tx/test_fec_tx.c ../apps/pivid-tx/src/fec_tx.c ../common/fec.c ../common/protocol.c
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -I../common -o $@ \
	    tx/test_fec_tx.c ../apps/pivid-tx/src/fec_tx.c ../common/fec.c ../common/protocol.c

tx/test_sps_reinject: tx/test_sps_reinject.c \
                   ../apps/pivid-tx/src/sps_reinject.c ../apps/pivid-tx/src/sps_reinject.h
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -o $@ \
	    tx/test_sps_reinject.c ../apps/pivid-tx/src/sps_reinject.c

# test_msp_parse exercises the pure MSPv1 wire parser (no UART/daemon deps).
tx/test_msp_parse: tx/test_msp_parse.c ../apps/pivid-tx/src/msp_parse.c ../apps/pivid-tx/src/msp_parse.h
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -o $@ tx/test_msp_parse.c ../apps/pivid-tx/src/msp_parse.c

# test_msp_request exercises the pure MSPv1 request builder (no UART/daemon deps).
tx/test_msp_request: tx/test_msp_request.c ../apps/pivid-tx/src/msp_request.c ../apps/pivid-tx/src/msp_request.h
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -o $@ tx/test_msp_request.c ../apps/pivid-tx/src/msp_request.c

# test_dp_client: pure DisplayPort sub-command parser into shadow grid (no UART).
tx/test_dp_client: tx/test_dp_client.c ../apps/pivid-tx/src/dp_client.c \
                ../apps/pivid-tx/src/dp_client.h ../common/osd_grid.c
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -I../common -o $@ \
	    tx/test_dp_client.c ../apps/pivid-tx/src/dp_client.c ../common/osd_grid.c

# test_camera_spawn: Linux integration -- fork/exec/pipe/reap/respawn against a
# stub child (PIVID_RPICAM_BIN). Links the pure argv builder it consumes.
tx/test_camera_spawn: tx/test_camera_spawn.c ../apps/pivid-tx/src/camera_spawn.c \
                ../apps/pivid-tx/src/camera_spawn.h ../common/camera_argv.c
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -I../common -o $@ \
	    tx/test_camera_spawn.c ../apps/pivid-tx/src/camera_spawn.c ../common/camera_argv.c

tx/test_aqm: tx/test_aqm.c ../apps/pivid-tx/src/aqm.c ../apps/pivid-tx/src/aqm.h \
          ../apps/pivid-tx/src/tx_csv.c ../common/radio_ioctl.h
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -I../common -o $@ \
	    tx/test_aqm.c ../apps/pivid-tx/src/aqm.c ../apps/pivid-tx/src/tx_csv.c

tx/test_tx_csv: tx/test_tx_csv.c ../apps/pivid-tx/src/tx_csv.c ../apps/pivid-tx/src/tx_csv.h
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -o $@ \
	    tx/test_tx_csv.c ../apps/pivid-tx/src/tx_csv.c

tx/test_fec_class: tx/test_fec_class.c ../apps/pivid-tx/src/fec_class.h
	$(CC) $(CFLAGS) -I../common -o $@ tx/test_fec_class.c

# ---- rx/ -----------------------------------------------------------------
rx/test_reasm: rx/test_reasm.c \
            ../apps/pivid-rx/src/reasm.c ../apps/pivid-rx/src/reasm.h \
            ../common/protocol.c
	$(CC) $(CFLAGS) -I../apps/pivid-rx/src -o $@ \
	    rx/test_reasm.c ../apps/pivid-rx/src/reasm.c ../common/protocol.c

# test_rx_loop drives rx_loop_drain_once against an in-memory fake ring.
rx/test_rx_loop: rx/test_rx_loop.c \
              ../apps/pivid-rx/src/rx_loop.c ../apps/pivid-rx/src/rx_loop.h \
              ../apps/pivid-rx/src/rx.c ../apps/pivid-rx/src/rx.h \
              ../apps/pivid-rx/src/reasm.c \
              ../apps/pivid-rx/src/fec_rx.c \
              ../apps/pivid-rx/src/osd_grid_rx.c \
              ../common/osd_grid.c \
              ../common/fec.c ../common/ring_user.c ../common/protocol.c
	$(CC) $(CFLAGS) -I../apps/pivid-rx/src -I../common -o $@ \
	    rx/test_rx_loop.c \
	    ../apps/pivid-rx/src/rx_loop.c ../apps/pivid-rx/src/rx.c \
	    ../apps/pivid-rx/src/reasm.c ../apps/pivid-rx/src/fec_rx.c \
	    ../apps/pivid-rx/src/osd_grid_rx.c ../common/osd_grid.c \
	    ../common/fec.c ../common/ring_user.c ../common/protocol.c

rx/test_fec_rx: rx/test_fec_rx.c ../apps/pivid-rx/src/fec_rx.c ../common/fec.c ../common/protocol.c
	$(CC) $(CFLAGS) -I../apps/pivid-rx/src -I../common -o $@ \
	    rx/test_fec_rx.c ../apps/pivid-rx/src/fec_rx.c ../common/fec.c ../common/protocol.c

# test_osd_grid_rx: RX seqlock grid reassembly + vid RSSI/LQ injection module.
rx/test_osd_grid_rx: rx/test_osd_grid_rx.c ../apps/pivid-rx/src/osd_grid_rx.c \
                  ../apps/pivid-rx/src/osd_grid_rx.h ../common/osd_grid.c
	$(CC) $(CFLAGS) -I../apps/pivid-rx/src -I../common -o $@ \
	    rx/test_osd_grid_rx.c ../apps/pivid-rx/src/osd_grid_rx.c ../common/osd_grid.c -lpthread

# ---- driver/ -------------------------------------------------------------
# test_ring uses only the static-inline pure helpers from lr2021_ring.h.
driver/test_ring: driver/test_ring.c ../drivers/lr2021/lr2021_ring.h ../common/radio_ioctl.h
	$(CC) $(CFLAGS) -o $@ driver/test_ring.c

# test_frame_sync uses only the static-inline pure helpers from lr2021_frame_sync.h.
driver/test_frame_sync: driver/test_frame_sync.c ../drivers/lr2021/lr2021_frame_sync.h
	$(CC) $(CFLAGS) -o $@ driver/test_frame_sync.c

# test_rx_walk: pure multi-packet stream walker (coalesced RX_DONE handling).
driver/test_rx_walk: driver/test_rx_walk.c ../drivers/lr2021/lr2021_rx_walk.h \
                     ../drivers/lr2021/lr2021_frame_sync.h
	$(CC) $(CFLAGS) -o $@ driver/test_rx_walk.c

# test_flrc_cfg uses only the static-inline encoder from lr2021_flrc_cfg.h.
driver/test_flrc_cfg: driver/test_flrc_cfg.c ../drivers/lr2021/lr2021_flrc_cfg.h ../common/radio_ioctl.h
	$(CC) $(CFLAGS) -o $@ driver/test_flrc_cfg.c

# test_rf_cfg uses only the static-inline encoders from lr2021_rf_cfg.h.
driver/test_rf_cfg: driver/test_rf_cfg.c ../drivers/lr2021/lr2021_rf_cfg.h
	$(CC) $(CFLAGS) -o $@ driver/test_rf_cfg.c

# test_status uses only the static-inline pure decoders from lr2021_status.h.
driver/test_status: driver/test_status.c ../drivers/lr2021/lr2021_status.h
	$(CC) $(CFLAGS) -o $@ driver/test_status.c

# ---- agent/ --------------------------------------------------------------
agent/test_sysstat: agent/test_sysstat.c ../apps/pivid-config-agent/src/sysstat.c
	$(CC) $(CFLAGS) -o $@ $^

agent/test_statusfile: agent/test_statusfile.c ../apps/pivid-config-agent/src/statusfile.c
	$(CC) $(CFLAGS) -o $@ $^

agent/test_form_parse: agent/test_form_parse.c ../apps/pivid-config-agent/src/form_parse.c \
                 ../common/settings.c ../common/phrase_sync.c ../common/sha256.c ../common/config_schema.c
	$(CC) $(CFLAGS) -I../apps/pivid-config-agent/src -o $@ $^

agent/test_modprobe_gen: agent/test_modprobe_gen.c ../apps/pivid-config-agent/src/modprobe_gen.c \
                   ../common/settings.c ../common/phrase_sync.c ../common/sha256.c ../common/config_schema.c
	$(CC) $(CFLAGS) -I../apps/pivid-config-agent/src -o $@ $^

agent/test_apply_engine: agent/test_apply_engine.c \
                   ../apps/pivid-config-agent/src/apply_engine.c \
                   ../common/config_schema.c ../common/settings.c \
                   ../common/phrase_sync.c ../common/sha256.c
	$(CC) $(CFLAGS) -I../apps/pivid-config-agent/src -o $@ $^

agent/test_schema_json: agent/test_schema_json.c ../apps/pivid-config-agent/src/schema_json.c \
                  ../common/config_schema.c ../common/settings.c \
                  ../common/phrase_sync.c ../common/sha256.c
	$(CC) $(CFLAGS) -I../apps/pivid-config-agent/src -o $@ $^

# ---- player/ -------------------------------------------------------------
# test_player_args: pure CLI parser for pivid-player (switch-only, no config).
player/test_player_args: player/test_player_args.c \
                ../apps/pivid-player/src/player_args.c \
                ../apps/pivid-player/src/player_args.h
	$(CC) $(CFLAGS) -I../apps/pivid-player/src -o $@ \
	    player/test_player_args.c ../apps/pivid-player/src/player_args.c

# test_player_state: pure connection state machine (no sockets/SDL/libav).
player/test_player_state: player/test_player_state.c ../apps/pivid-player/src/player_state.c ../apps/pivid-player/src/player_state.h
	$(CC) $(CFLAGS) -I../apps/pivid-player/src -o $@ player/test_player_state.c ../apps/pivid-player/src/player_state.c

# test_mdns_parse: pure mDNS response parser (no sockets/threads). Golden Avahi
# packet + crafted edge cases (cache-flush bit, A-before-SRV, truncation, loops).
player/test_mdns_parse: player/test_mdns_parse.c \
                ../apps/pivid-player/src/mdns_parse.c \
                ../apps/pivid-player/src/mdns_parse.h
	$(CC) $(CFLAGS) -I../apps/pivid-player/src -o $@ \
	    player/test_mdns_parse.c ../apps/pivid-player/src/mdns_parse.c

# test_osd_glyph_cache: integer-keyed Cairo surface cache (needs Cairo).
player/test_osd_glyph_cache: player/test_osd_glyph_cache.c \
                ../apps/pivid-player/src/osd_glyph_cache.c \
                ../apps/pivid-player/src/osd_glyph_cache.h
	$(CC) $(CFLAGS) -I../apps/pivid-player/src \
	    $(shell pkg-config --cflags cairo) -o $@ \
	    player/test_osd_glyph_cache.c ../apps/pivid-player/src/osd_glyph_cache.c \
	    $(shell pkg-config --libs cairo)

# test_osd_msp_font: in-memory PNG -> cairo sheet loader (needs Cairo).
# player/font_stub.c satisfies the font_btfl_dji_png[] symbol (CMake-only embed).
player/test_osd_msp_font: player/test_osd_msp_font.c ../apps/pivid-player/src/osd_msp_font.c \
                ../apps/pivid-player/src/osd_msp_font.h player/font_stub.c
	$(CC) $(CFLAGS) -I../apps/pivid-player/src $(shell pkg-config --cflags cairo) -o $@ \
	    player/test_osd_msp_font.c ../apps/pivid-player/src/osd_msp_font.c \
	    player/font_stub.c \
	    $(shell pkg-config --libs cairo)

player/test_lan_reasm: player/test_lan_reasm.c ../apps/pivid-player/src/lan_reasm.c ../apps/pivid-player/src/lan_reasm.h ../common/lanfrag.h
	$(CC) $(CFLAGS) -o $@ player/test_lan_reasm.c ../apps/pivid-player/src/lan_reasm.c

player/test_au_asm: player/test_au_asm.c ../apps/pivid-player/src/au_asm.c ../apps/pivid-player/src/au_asm.h
	$(CC) $(CFLAGS) -I../apps/pivid-player/src -o $@ \
	    player/test_au_asm.c ../apps/pivid-player/src/au_asm.c

player/test_lan_source_core: player/test_lan_source_core.c \
                ../apps/pivid-player/src/lan_source.c \
                ../apps/pivid-player/src/lan_reasm.c \
                ../apps/pivid-player/src/au_asm.c ../common/lanfrag.h
	$(CC) $(CFLAGS) -I../apps/pivid-player/src -I../common -o $@ \
	    player/test_lan_source_core.c ../apps/pivid-player/src/lan_source.c \
	    ../apps/pivid-player/src/lan_reasm.c ../apps/pivid-player/src/au_asm.c

# test_frame_gate: pure-pthread waitable generation counter (no libav/SDL).
player/test_frame_gate: player/test_frame_gate.c \
                ../apps/pivid-player/src/frame_gate.c \
                ../apps/pivid-player/src/frame_gate.h
	$(CC) $(CFLAGS) -pthread -I../apps/pivid-player/src -o $@ \
	    player/test_frame_gate.c ../apps/pivid-player/src/frame_gate.c

# ---- caster/ -------------------------------------------------------------
# test_caster_control: pure LE control-protocol codec (now in common/).
caster/test_caster_control: caster/test_caster_control.c ../common/control.c ../common/control.h
	$(CC) $(CFLAGS) -o $@ caster/test_caster_control.c ../common/control.c

# test_caster_subscribers: pure subscriber-table logic (no sockets).
caster/test_caster_subscribers: caster/test_caster_subscribers.c ../apps/pivid-caster/src/subscribers.c ../apps/pivid-caster/src/subscribers.h
	$(CC) $(CFLAGS) -I../apps/pivid-caster/src -o $@ caster/test_caster_subscribers.c ../apps/pivid-caster/src/subscribers.c

# test_caster_egress: egress hardening (non-blocking fwd_fd, bumped ingest
# rcvbuf, telem send-fail counting). Drives caster_step() synchronously (no
# threads/timing) but binds the fixed caster ports, so it runs sequentially.
caster/test_caster_egress: caster/test_caster_egress.c ../apps/pivid-caster/src/caster.c ../apps/pivid-caster/src/subscribers.c ../common/control.c
	$(CC) $(CFLAGS) -I../apps/pivid-caster/src -o $@ caster/test_caster_egress.c ../apps/pivid-caster/src/caster.c ../apps/pivid-caster/src/subscribers.c ../common/control.c

# ---- integration/ --------------------------------------------------------
# Built with the widened block-interleaver dims (-DFEC_TX_RING_MAX=16
# -DFEC_RX_SLOTS=32) so the cross-NAL block-interleave discriminator can exercise
# depth 4. The depth-OFF byte-identical guard inside this test, plus tx/test_fec_tx
# at the DEFAULT (production) dims, together prove production is untouched.
integration/test_fec_e2e: integration/test_fec_e2e.c \
              ../apps/pivid-tx/src/fec_tx.c \
              ../apps/pivid-rx/src/fec_rx.c \
              ../apps/pivid-rx/src/reasm.c \
              ../common/fec.c ../common/protocol.c
	$(CC) $(CFLAGS) -DFEC_TX_RING_MAX=16 -DFEC_RX_SLOTS=32 \
	    -I../apps/pivid-tx/src -I../apps/pivid-rx/src -I../common -o $@ \
	    integration/test_fec_e2e.c ../apps/pivid-tx/src/fec_tx.c ../apps/pivid-rx/src/fec_rx.c \
	    ../apps/pivid-rx/src/reasm.c ../common/fec.c ../common/protocol.c

# test_frame_sync_fec is the cross-layer guard: kernel self-sync below the
# FEC layer (fec_tx -> framing -> drop -> fec_rx -> reasm).
integration/test_frame_sync_fec: integration/test_frame_sync_fec.c \
                     ../apps/pivid-tx/src/fec_tx.c \
                     ../apps/pivid-rx/src/fec_rx.c \
                     ../apps/pivid-rx/src/reasm.c \
                     ../drivers/lr2021/lr2021_frame_sync.h \
                     ../common/fec.c ../common/protocol.c
	$(CC) $(CFLAGS) -I../apps/pivid-tx/src -I../apps/pivid-rx/src -I../common -o $@ \
	    integration/test_frame_sync_fec.c ../apps/pivid-tx/src/fec_tx.c ../apps/pivid-rx/src/fec_rx.c \
	    ../apps/pivid-rx/src/reasm.c ../common/fec.c ../common/protocol.c

# --------------------------------------------------------------------------
run: $(TEST_TARGETS)
	@set -e; for t in $(TEST_TARGETS); do echo "== $$t =="; ./$$t; done
	bash agent/test_config_net.sh
	bash agent/test_config_units.sh
	bash agent/test_wifi_client.sh

clean:
	rm -f $(TEST_TARGETS)
