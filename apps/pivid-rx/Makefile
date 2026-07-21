# apps/pivid-rx/Makefile -- cross-compile the RX userspace daemon.
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
               $(COMMON_DIR)/osd_grid.c
ifdef LATPROBE
CFLAGS += -DLATPROBE
COMMON_SRCS += $(COMMON_DIR)/resid_hist.c
endif
APP_SRCS := src/main.c src/rx.c src/reasm.c src/fec_rx.c src/output_udp.c src/rx_loop.c src/gpio_thread.c src/osd_grid_rx.c src/telem_tx.c
OBJS := $(COMMON_SRCS:.c=.o) $(APP_SRCS:.c=.o)
TARGET := ../../bin/pivid-rx

LDLIBS := -lgpiod -lm -lpthread

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
