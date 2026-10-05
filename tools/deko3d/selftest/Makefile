# deko9 self-test NRO: src/deko9 driven through D3D9 only (see source/main.cpp).
#   make -C tools/deko3d/selftest [KISAK_SWITCH_LOG_HOST=<ip>] [DEKO3D_DEBUG=1]
# Needs the shader-compiler libraries from a configured switch-sp build tree
# (DEPS_BUILD; `cmake --build <tree> --target uam mojoshader`) and the vendored
# D3D9/Windows headers in deps/d3d9-headers (see its README.md).
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the devkitPro installation)
endif

TOPDIR := $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

# Computed once in the outer make (the inner one runs from build/).
REPO ?= $(abspath $(CURDIR)/../../..)
DEPS_BUILD ?= $(REPO)/build/switch-sp
export REPO DEPS_BUILD
D3D9HEADERS ?= $(REPO)/deps/d3d9-headers
MOJO ?= $(DEPS_BUILD)/_deps/mojoshader-src
UAM ?= $(DEPS_BUILD)/_deps/uam-src

# LTO=1 builds the self-test as one LTO unit, the way KISAK_SWITCH_LTO links
# src/deko9 into the game; it gets its own build dir and target name because
# make does not track flag changes.
ifeq ($(LTO),1)
TARGET := KisakCOD-deko9-selftest-lto
BUILD := build-lto
else
TARGET := KisakCOD-deko9-selftest
BUILD := build
endif
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
DEFS := -D__SWITCH__ -D__cdecl= -D__stdcall= '-D__declspec(x)='
CFLAGS := -g -O2 -Wall -Wextra -Wno-unused-parameter -ffunction-sections $(ARCH) $(DEFS)
ifneq ($(strip $(KISAK_SWITCH_LOG_HOST)),)
CFLAGS += -DKISAK_SWITCH_LOG_HOST=\"$(KISAK_SWITCH_LOG_HOST)\"
endif
CXXFLAGS = $(CFLAGS) $(INCLUDE) -std=gnu++20
ASFLAGS := -g $(ARCH)
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
ifeq ($(LTO),1)
CFLAGS += -flto=auto
LDFLAGS += -flto=auto -O2
endif
DKLIB := -ldeko3d
ifeq ($(DEKO3D_DEBUG),1)
DKLIB := -ldeko3dd
endif
LIBS := $(DEPS_BUILD)/lib/libuam.a $(DEPS_BUILD)/lib/libmojoshader.a $(DKLIB) -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(CURDIR)/source:$(REPO)/src/deko9:$(REPO)/src/platform/switch:$(REPO)/src/port
export DEPSDIR := $(CURDIR)/$(BUILD)
export LD := $(CXX)
# switch_perf.o: deko9 feeds the port profiler's frame extras; it stays off (SwitchPerf_g_enabled 0) here.
export OFILES := main.o taau_test.o deko9_callcensus.o deko9_drawcensus.o deko9_d3d.o deko9_device.o deko9_draw.o deko9_fsr.o deko9_fsr_shaders.o deko9_gpufault.o \
	deko9_memory.o deko9_resources.o deko9_shader.o deko9_state_map.o deko9_taau.o deko9_taau_shaders.o deko9_zcull.o switch_port_log.o \
	switch_perf.o
export INCLUDE := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD) \
	-I$(D3D9HEADERS)/directx -I$(D3D9HEADERS)/windows -I$(D3D9HEADERS) \
	-I$(MOJO) -isystem $(UAM)/source -isystem $(UAM)/mesa-imported -I$(REPO)/src
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean
all: | $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).elf $(TARGET).nacp

else

DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)

endif
