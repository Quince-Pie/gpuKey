# GNU Make >= 4.3.  Nix sets CC, PKG_CONFIG and prefix; everything else here is
# a plain variable you can override on the command line.
#
#   make                        debug build         -> build/gcc-debug/gpukey
#   make PROFILE=release        optimised build     -> build/gcc-release/gpukey
#   make TRACY=1                instrumented build  -> build/gcc-debug-tracy/gpukey
#   make SANITIZE=address,undefined
#   make check                  build and run tests/
#   make compile_commands.json  for clangd
#   make install prefix=$HOME/.local

.RECIPEPREFIX := >
.DELETE_ON_ERROR:
.SUFFIXES:
MAKEFLAGS += --no-builtin-rules

PROFILE   ?= debug
TRACY     ?= 0
SANITIZE  ?=
LTO       ?= 0
VPP_CONTROL ?= 0
REPLAY_EVENTFD ?= 0
PORTAL_READY_TIME ?= 0
CPU_CONVERSION_CONTROL ?= 0
VULKAN_RGB ?= 0
# The dev shells export this (gcc, clang-mold, ...); elsewhere it is the compiler name.
TOOLCHAIN ?= $(notdir $(CC))

CC         ?= cc
PKG_CONFIG ?= pkg-config
INSTALL    ?= install
prefix     ?= /usr/local
bindir     ?= $(prefix)/bin

name := gpukey

ifeq ($(filter $(PROFILE),debug release),)
  $(error PROFILE must be debug or release, not '$(PROFILE)')
endif

# Every configuration gets its own tree, so switching does not rebuild the other.
comma    := ,
builddir := build/$(TOOLCHAIN)-$(PROFILE)$(if $(filter 1,$(TRACY)),-tracy)$(if $(filter 1,$(VPP_CONTROL)),-vpp-control)$(if $(filter 1,$(REPLAY_EVENTFD)),-replay-eventfd)$(if $(filter 1,$(PORTAL_READY_TIME)),-portal-ready-time)$(if $(filter 1,$(CPU_CONVERSION_CONTROL)),-sws-control)$(if $(filter 1,$(VULKAN_RGB)),-rgb)$(if $(SANITIZE),-$(subst $(comma),-,$(SANITIZE)))

# ---- flags -------------------------------------------------------------------
# Project flags live in lowercase variables.  CPPFLAGS, CFLAGS, LDFLAGS and
# LDLIBS stay yours and are appended last, so they win.
warnings := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wdouble-promotion \
            -Wvla -Wstrict-prototypes -Wmissing-prototypes -Wundef \
            -Werror=implicit-function-declaration \
            -Werror=incompatible-pointer-types -Werror=return-type
cflags.debug   := -O0 -g3
cflags.release := -O2 -g -DNDEBUG
# Constant filter loops benefit from loop unswitching/vectorization. Keep the
# policy local to the conversion unit and let caller CFLAGS remain last.
convert_optimization := $(if $(filter release,$(PROFILE)),-O3)

packages := libpipewire-0.3 gio-unix-2.0 libavcodec libavformat libavutil libswresample libswscale libva libva-drm libdrm liburing
ifeq ($(VULKAN_RGB),1)
  packages += vulkan
endif
dependency_flags := $(shell $(PKG_CONFIG) --cflags $(packages))
cppflags := -Isrc -D_GNU_SOURCE $(patsubst -I%,-isystem %,$(dependency_flags))
cflags   := -std=c23 $(warnings) $(cflags.$(PROFILE)) -fno-omit-frame-pointer
ldflags  :=
ifeq ($(VULKAN_RGB),1)
  cppflags += -DGK_VULKAN_RGB
endif
ldlibs   := $(shell $(PKG_CONFIG) --libs $(packages)) -pthread -lm
ifneq ($(CPU_CONVERSION_CONTROL),1)
  ldlibs := $(filter-out -lswscale,$(ldlibs))
endif
ifeq ($(VPP_CONTROL),1)
  cppflags += -DGK_TRANSIENT_VPP
endif
ifeq ($(REPLAY_EVENTFD),1)
  cppflags += -DGK_REPLAY_EVENTFD
endif
ifeq ($(PORTAL_READY_TIME),1)
  cppflags += -DGK_PORTAL_READY_TIME
endif
ifeq ($(CPU_CONVERSION_CONTROL),1)
  cppflags += -DGK_CPU_CONVERSION_CONTROL
endif

# The Tracy headers are always on the include path: TracyC.h expands to nothing
# unless TRACY_ENABLE is defined, which only pkg-config's full --cflags do.
ifeq ($(TRACY),1)
  cppflags += $(shell $(PKG_CONFIG) --cflags tracy)
  ldlibs   += $(shell $(PKG_CONFIG) --libs tracy)
else
  cppflags += $(shell $(PKG_CONFIG) --cflags-only-I tracy)
endif

ifneq ($(SANITIZE),)
  cflags  += -fsanitize=$(SANITIZE) -fno-sanitize-recover=all
  ldflags += -fsanitize=$(SANITIZE)
endif
ifeq ($(LTO),1)
  cflags  += -flto
  ldflags += -flto
endif

cppflags += $(CPPFLAGS)
cflags_base := $(cflags)
cflags = $(cflags_base) $(unit_optimization) $(CFLAGS)
ldflags  += $(LDFLAGS)
ldlibs_base := $(ldlibs)
ldlibs = $(ldlibs_base) $(probe_libraries) $(LDLIBS)
conversion_probe_libs := $(shell $(PKG_CONFIG) --libs libswscale)
conversion_fallback_flags := -Wl,--wrap=av_malloc

# ---- graph -------------------------------------------------------------------
srcs := $(wildcard src/*.c)
objs := $(srcs:src/%.c=$(builddir)/%.o)
bin  := $(builddir)/$(name)

test_srcs := $(wildcard tests/*_test.c)
test_bins := $(test_srcs:tests/%.c=$(builddir)/tests/%)
lib_objs  := $(filter-out $(builddir)/main.o,$(objs))

# Make compares timestamps, not command lines.  Record the resolved compiler
# and every flag, refresh the record only when it differs, and hang all
# compiles and links off it: a new wrapper, linker or flag rebuilds what it
# affects, and nothing else.
config      := $(TOOLCHAIN) $(shell command -v $(CC)) | $(cppflags) $(cflags) | convert=$(convert_optimization) | probes=$(conversion_probe_libs) $(conversion_fallback_flags) | $(ldflags) $(ldlibs) | $(srcs) $(test_srcs)
config_file := $(builddir)/config
$(shell mkdir -p $(builddir); [ "$$(cat $(config_file) 2>/dev/null)" = '$(config)' ] || printf '%s\n' '$(config)' > $(config_file))

.PHONY: all check feature-check integration hardware-check install clean
all: $(bin)

$(objs) $(bin) $(test_bins): $(config_file)

$(bin): $(objs) | $(builddir)
> $(CC) $(ldflags) -o $@ $(objs) $(ldlibs)

# Each compile also records its command line for compile_commands.json.
$(builddir)/%.o: src/%.c | $(builddir)
> $(CC) $(cppflags) $(cflags) -MMD -MP -MF $(@:.o=.d) -c -o $@ $<
> @printf '{"directory":"%s","file":"%s","output":"%s","command":"%s"}\n' \
>   "$(CURDIR)" "$<" "$@" \
>   "$$(printf '%s' '$(CC) $(cppflags) $(cflags) -c -o $@ $<' | sed 's/["\\]/\\&/g')" \
>   > $(@:.o=.json)

$(builddir)/convert.o: private unit_optimization := $(convert_optimization)

$(builddir)/tests/%: tests/%.c $(lib_objs) | $(builddir)/tests
> $(CC) $(cppflags) $(cflags) -MMD -MP -MF $@.d $(ldflags) -o $@ $< $(lib_objs) $(ldlibs)

# The comparison probe alone calls the alternate swscale implementations.
$(builddir)/tests/conversion_probe: private probe_libraries := $(conversion_probe_libs)
$(builddir)/tests/conversion_fallback_test: private probe_libraries := $(conversion_fallback_flags)

check: $(bin) $(test_bins)
> @for t in $(test_bins); do echo "run $$t"; "$$t" || exit 1; done
> bash tests/cli.sh $(abspath $(bin))
> bash tests/replay-cli.sh $(abspath $(bin))
> bash tests/validation_gate.sh $(abspath tools/check-vulkan-dpb.sh)

integration: $(bin) $(builddir)/tests/capture_fixture
> bash tests/integration.sh $(abspath $(bin)) $(abspath $(builddir)/tests/capture_fixture)

feature-check: $(builddir)/tests/codec_features_probe
> $(builddir)/tests/codec_features_probe

hardware-check: $(bin) $(builddir)/tests/gpu_probe $(builddir)/tests/gpu_contexts_probe $(builddir)/tests/initial_output_probe
> $(builddir)/tests/gpu_probe
> $(builddir)/tests/initial_output_probe
> @scratch=$$(mktemp -d) || exit; trap 'rm -rf "$$scratch"' EXIT; \
>   $(builddir)/tests/gpu_contexts_probe "$$scratch"

compile_commands.json: $(objs)
> { echo '['; cat $(objs:.o=.json) | paste -sd, -; echo ']'; } > $@

install: $(bin)
> $(INSTALL) -Dm755 $(bin) $(DESTDIR)$(bindir)/$(name)

clean:
> rm -rf build compile_commands.json

$(builddir) $(builddir)/tests:
> mkdir -p $@

-include $(objs:.o=.d) $(wildcard $(builddir)/tests/*.d)
