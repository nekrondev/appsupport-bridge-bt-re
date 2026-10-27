# Build the appsupport-bridge-bt reimplementation for aarch64 Sailfish OS.
#
# Dependency modes (MODE=...):
#   vendored-release   (recommended for CI) link glib from the distro + the Sailfish-specific
#                      libgbinder/libglibutil vendored in ./vendor (pinned to an OS release).
#                      Build natively (CROSS=) in an aarch64 environment.
#   pkgconfig          all deps via pkg-config (host has glib + libgbinder + libglibutil -dev).
#   vendored (default) the monica cross-build: glib headers via pkg-config, gbinder/glibutil
#                      headers from $(BB)/src, link against device .so in $(BB)/sysroot/lib.
#
# Cross prefix (empty for a native aarch64 build, e.g. in an arm64 container):  make CROSS=

MODE   ?= vendored
CROSS  ?= aarch64-linux-gnu-
CC      = $(CROSS)gcc

CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS += -Isrc
GLIB   := glib-2.0 gobject-2.0 gio-2.0

ifeq ($(MODE),vendored-release)
  CFLAGS  += $(shell pkg-config --cflags $(GLIB)) -Ivendor/include
  LDFLAGS += -Lvendor/lib -Wl,-rpath-link,vendor/lib
  LIBS    ?= -l:libgbinder.so.1 -l:libglibutil.so.1 $(shell pkg-config --libs $(GLIB))
else ifeq ($(MODE),pkgconfig)
  PKGS    := $(GLIB) libgbinder libglibutil
  CFLAGS  += $(shell pkg-config --cflags $(PKGS))
  LIBS    ?= $(shell pkg-config --libs $(PKGS))
else
  BB      ?= $(HOME)/bridge-build
  CFLAGS  += $(shell pkg-config --cflags $(GLIB)) \
             -I$(BB)/src/libgbinder/include -I$(BB)/src/libglibutil/include
  LDFLAGS += -L$(BB)/sysroot/lib -Wl,-rpath-link,$(BB)/sysroot/lib
  LIBS    ?= -lgbinder -lglibutil -lglib-2.0 -lgobject-2.0 -lgio-2.0
endif

SRC := src/main.c src/hci.c src/gbinder_svc.c src/vhci.c src/mux.c src/config.c src/rfkill.c
OBJ := $(SRC:.c=.o)
BIN := appsupport-bridge-bt

# Header paths for the unit test (compiled + run NATIVELY with host cc)
TESTINC := $(shell pkg-config --cflags glib-2.0)
ifeq ($(MODE),pkgconfig)
  TESTINC += $(shell pkg-config --cflags libgbinder libglibutil)
else ifeq ($(MODE),vendored-release)
  TESTINC += -Ivendor/include
else
  TESTINC += -I$(BB)/src/libgbinder/include -I$(BB)/src/libglibutil/include
endif

all: $(BIN)

# Unit-test the mux/virtualization logic off-device (host-native, arch-independent).
test:
	cc test/mux_test.c -Isrc $(TESTINC) $(shell pkg-config --libs glib-2.0) -o /tmp/mux_test
	/tmp/mux_test

$(BIN): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) $(LIBS) -o $@
	@echo "=== built: $$(file $@ | cut -d, -f1-2) ==="

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean test
