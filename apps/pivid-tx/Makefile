# apps/pivid-tx/Makefile -- cross-compile the TX userspace daemon.
# Run inside the pivid-builder image (sets ARCH/CROSS_COMPILE via env).

CC := aarch64-linux-gnu-gcc
CFLAGS := -Wall -Wextra -Wpedantic -std=c11 -O2 -pthread
CFLAGS += -I../../common -I.
# -MMD -MP: emit per-object .d header-dependency files so a changed .h
# rebuilds the .o that includes it (the generic %.o rule alone misses this).
CFLAGS += -MMD -MP
# Build stamp (top Makefile passes -e BUILD_VERSION; "unknown" for a bare build).
BUILD_VERSION ?= unknown
CFLAGS += -DBUILD_VERSION=\"$(BUILD_VERSION)\"
ifdef LATPROBE
CFLAGS += -DLATPROBE
endif

COMMON_DIR := ../../common
COMMON_SRCS := $(COMMON_DIR)/protocol.c \
               $(COMMON_DIR)/settings.c \
               $(COMMON_DIR)/cli_args.c \
               $(COMMON_DIR)/phrase_sync.c \
               $(COMMON_DIR)/sha256.c \
               $(COMMON_DIR)/config_schema.c \
               $(COMMON_DIR)/peripherals.c \
               $(COMMON_DIR)/ring_user.c \
               $(COMMON_DIR)/fec.c \
               $(COMMON_DIR)/status_file.c \
               $(COMMON_DIR)/pkt_hist.c \
               $(COMMON_DIR)/resid_hist.c \
               $(COMMON_DIR)/osd_grid.c \
               $(COMMON_DIR)/camera_argv.c
APP_SRCS := src/main.c src/tx.c src/tx_sample.c src/tx_csv.c src/aqm.c src/encoder.c src/fec_tx.c src/sps_reinject.c src/gpio_thread.c \
            src/msp_poll.c src/msp_parse.c src/msp_request.c src/dp_client.c src/camera_spawn.c
OBJS := $(COMMON_SRCS:.c=.o) $(APP_SRCS:.c=.o)
TARGET := ../../bin/pivid-tx

LDLIBS := -lgpiod

.PHONY: all clean FORCE

all: $(TARGET)

# main.o carries the BUILD_VERSION build stamp; force it to rebuild every build so
# the stamp tracks the current tree (a -D change alone won't trigger a rebuild).
src/main.o: FORCE
FORCE:

$(TARGET): $(OBJS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
	aarch64-linux-gnu-strip $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
