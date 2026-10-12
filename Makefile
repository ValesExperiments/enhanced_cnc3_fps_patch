ZIG ?= zig
PYTHON ?= python3
TARGET := x86-windows-gnu

SOURCES := \
	src_fps_patch/common.c \
	src_fps_patch/game_layout.c \
	src_fps_patch/game_patches.c \
	src_fps_patch/memory_patch.c \
	src_fps_patch/config.c \
	src_fps_patch/log.c \
	src_fps_patch/runtime.c \
	src_fps_patch/proxy_dinput8.c \
	src_fps_patch/dllmain.c

HEADERS := $(wildcard src_fps_patch/*.h)

COMMON_CFLAGS := \
	-target $(TARGET) \
	-std=c11 \
	-Wall \
	-Wextra \
	-Werror \
	-Isrc_fps_patch

COMMON_LDFLAGS := \
	-target $(TARGET) \
	-shared \
	-Wl,--subsystem,windows \
	-Wl,--major-subsystem-version,6 \
	-Wl,--minor-subsystem-version,1 \
	-Wl,--dynamicbase \
	-Wl,--nxcompat

MODE ?= release
OBJDIR := zig-out/obj/$(MODE)
BINDIR := zig-out/bin
LIBDIR := zig-out/lib
OBJECTS := $(patsubst src_fps_patch/%.c,$(OBJDIR)/%.obj,$(SOURCES))

ifeq ($(MODE),debug)
  CFLAGS := $(COMMON_CFLAGS) -O0 -g
  LDFLAGS := $(COMMON_LDFLAGS) -O0 -g
  LINK := $(ZIG) cc
  OUTPUT := $(BINDIR)/dinput8-debug.dll
else
  CFLAGS := $(COMMON_CFLAGS) -O2 -DNDEBUG
  LDFLAGS := $(COMMON_LDFLAGS) -O2
  LINK := $(PYTHON) scripts/link_release.py $(ZIG) cc
  LINK_DEPS := scripts/link_release.py
  OUTPUT := $(BINDIR)/dinput8.dll
endif

.PHONY: all release debug build package clean

all: release

release:
	$(MAKE) MODE=release build

debug:
	$(MAKE) MODE=debug build

build: $(OUTPUT) $(BINDIR)/fps_patch.ini

# Ship the default configuration next to the DLL so zig-out/bin is install-ready.
$(BINDIR)/fps_patch.ini: fps_patch.ini.example | $(BINDIR)
	cp $< $@

$(OUTPUT): $(OBJECTS) src_fps_patch/dinput8.def $(LINK_DEPS) | $(BINDIR) $(LIBDIR)
	$(LINK) $(LDFLAGS) \
		-Wl,/implib:$(LIBDIR)/dinput8-$(MODE).lib \
		-Wl,/pdb:$(LIBDIR)/dinput8-$(MODE).pdb \
		$(OBJECTS) src_fps_patch/dinput8.def -lkernel32 -lwinmm -o $@

$(OBJDIR)/%.obj: src_fps_patch/%.c $(HEADERS) | $(OBJDIR)
	$(ZIG) cc $(CFLAGS) -c $< -o $@

$(OBJDIR) $(BINDIR) $(LIBDIR):
	mkdir -p $@

package: release
	rm -rf zig-out/package
	mkdir -p zig-out/package
	cp zig-out/bin/dinput8.dll zig-out/package/dinput8.dll
	cp fps_patch.ini.example zig-out/package/fps_patch.ini
	cp README.md zig-out/package/README.md

clean:
	rm -rf zig-out
