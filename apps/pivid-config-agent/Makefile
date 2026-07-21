# apps/pivid-config-agent/Makefile -- cross-compile the always-on config agent.
# Run inside pivid-builder. civetweb is compiled with relaxed warnings (it does
# not pass -Wpedantic); our own code stays strict.
#
# NOTE: upstream src/civetweb.c is NOT a self-contained amalgamation -- it
# #includes helper translation units (md5.inl, sort.inl, match.inl,
# response.inl, handle_form.inl) unconditionally. Those are vendored verbatim
# alongside civetweb.c/.h in third_party/civetweb/ so the relative includes
# resolve. SSL/CGI/Lua/zlib .inl files are gated off by the feature flags below
# and are intentionally not vendored.

CC := aarch64-linux-gnu-gcc
CIVET := third_party/civetweb
CFLAGS := -Wall -Wextra -Wpedantic -std=c11 -O2 -pthread
CFLAGS += -I../../common -Isrc -I$(CIVET)
CFLAGS += -MMD -MP

# Build stamp (passed in from the top Makefile via -e BUILD_VERSION; "unknown"
# for a bare in-app build). Surfaced in the dashboard System panel.
BUILD_VERSION ?= unknown
CFLAGS += -DBUILD_VERSION=\"$(BUILD_VERSION)\"

WEBUI_H := src/webui.h
WEB_SRC := web/index.html
WEB_CSS := web/pico.classless.min.css

# civetweb feature flags: plain HTTP only, no on-disk docroot, no CGI.
CIVET_DEFS := -DNO_SSL -DNO_CGI -DNO_FILES -DNO_CACHING -DUSE_IPV6=0
CIVET_CFLAGS := -std=c11 -O2 -pthread -I$(CIVET) $(CIVET_DEFS) -w

COMMON_DIR := ../../common
COMMON_SRCS := $(COMMON_DIR)/settings.c $(COMMON_DIR)/cli_args.c \
               $(COMMON_DIR)/phrase_sync.c \
               $(COMMON_DIR)/sha256.c $(COMMON_DIR)/config_schema.c \
               $(COMMON_DIR)/peripherals.c $(COMMON_DIR)/link_capacity.c \
               $(COMMON_DIR)/recipe_defaults.c
APP_SRCS := src/main.c src/role.c src/button_led.c src/sysstat.c src/statusfile.c \
            src/form_parse.c src/modprobe_gen.c src/apply_engine.c src/schema_json.c src/http.c
OBJS := $(COMMON_SRCS:.c=.o) $(APP_SRCS:.c=.o) $(CIVET)/civetweb.o
TARGET := ../../bin/pivid-config-agent

LDLIBS := -lgpiod -ldl -lrt

.PHONY: all clean FORCE
all: $(WEBUI_H) $(TARGET)

# main.o carries the BUILD_VERSION build stamp. A -D value change does not alter
# main.c's mtime, so make would not otherwise rebuild it -- force it every build
# so the stamp always tracks the current tree (cheap: small TU).
src/main.o: FORCE
FORCE:

$(WEBUI_H): $(WEB_SRC) $(WEB_CSS) ../../scripts/embed_asset.sh
	sh ../../scripts/embed_asset.sh $(WEB_SRC) $(WEB_CSS) WEBUI_HTML > $@

# http.o embeds the generated dashboard (a ~77 KB string literal), which trips
# -Wpedantic's overlength-strings limit; suppress only for this TU.
src/http.o: src/http.c $(WEBUI_H)
	$(CC) $(CFLAGS) -Wno-overlength-strings -c -o $@ $<

$(TARGET): $(OBJS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
	aarch64-linux-gnu-strip $@

$(CIVET)/civetweb.o: $(CIVET)/civetweb.c
	$(CC) $(CIVET_CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(APP_SRCS:.c=.d) $(COMMON_SRCS:.c=.d)

clean:
	rm -f $(OBJS) $(APP_SRCS:.c=.d) $(COMMON_SRCS:.c=.d) $(TARGET) $(WEBUI_H)
