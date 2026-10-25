# polyend-doom: Doom as a standalone firmware image for the original Polyend Tracker.
#
#   make            -> build/PolyendTrackerDOOMv<N>.ptf (+ .hex/.elf/.map), size report
#   make host       -> build/host/doom-host (SDL2), the same Doom glue on the PC
#   make host-check -> headless scripted host runs (demo, new game, controls, jog walking)
#   make test       -> host unit tests (MUS -> MIDI sequencer, sound mixer)
#   make clean
#
# Board layer, Teensy core and FatFs come from OpenTracker (vendor/opentracker,
# src/main.cpp calls our otAppMain); the engine is doomgeneric (vendor/doomgeneric).
VERSION   ?= 3
ZONE_MB   ?= 10
BUILD     := build
OT        := vendor/opentracker
DG        := vendor/doomgeneric/doomgeneric
WAD       ?= $(HOME)/Projects/Doom/wad/doom1.wad

# The update bootloader only accepts 24-character image names (OpenTracker
# docs/HARDWARE_TESTING.md), and the stock updater lists "PolyendTrac*.ptf".
PTF_NAME  := PolyendTrackerDOOMv$(VERSION).ptf
ifneq ($(shell printf '%s' '$(PTF_NAME)' | wc -c | tr -d ' '),24)
$(error image name '$(PTF_NAME)' must be exactly 24 characters)
endif
PTF       := $(BUILD)/$(PTF_NAME)

# ---- Doom engine (doomgeneric), minus the files src/doom replaces -------------
DOOM_FILES := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net \
              f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound \
              i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc \
              m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats \
              p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw \
              r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables \
              v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input
DOOM_SRCS := $(addprefix $(DG)/,$(addsuffix .c,$(DOOM_FILES)))
GLUE_SRCS := $(wildcard src/doom/*.c)
DOOM_DEFS := -DFEATURE_SOUND -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE
DOOM_INCS := -I$(DG) -Isrc/doom -Isrc/doom/shim
# Doom's own warnings are not ours to fix.
DOOM_WARN := -w
WRAPS     := -Wl,--wrap=I_Quit,--wrap=I_Error

# ---- device ---------------------------------------------------------------------
CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
CXX       := $(CROSS)g++
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size
CORE      := $(OT)/third_party/teensy3

CPUFLAGS  := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
# OpenTracker's board defines (its Makefile), Games off, release defaults otherwise.
OT_DEFS   := -D__MK66FX1M0__ -DF_CPU=180000000 -DUSB_MIDI_SERIAL -DLAYOUT_US_ENGLISH -DF_BUS=90000000 \
             -DSERIAL5_TX_BUFFER_SIZE=512 -DSERIAL5_RX_BUFFER_SIZE=512 -DTEENSYDUINO=159 -DARDUINO=10819 \
             -DOT_VERSION=\"doom-$(VERSION)\" -DOT_TARGET_DEVICE=1 -DOT_SDRAM_CACHE=1 -DOT_GAMES=0 \
             -DOT_FASTRUN=1 -DOT_USB_MSC=1 -DOT_FONTS_COMPRESSED=1 -DOT_USB_VID=0x1209 -DOT_USB_PID=0x0001
PT_DEFS   := -DPT_VERSION=\"v$(VERSION)\" -DPT_IMAGE_NAME=\"$(PTF_NAME)\" -DPT_ZONE_MB=\"$(ZONE_MB)\"
OT_INCS   := -I$(CORE) -I$(OT)/src -I$(OT)/src/core -I$(OT)/src/hal -I$(OT)/third_party/fatfs
COMMON    := $(CPUFLAGS) $(OT_DEFS) $(PT_DEFS) -g -ffunction-sections -fdata-sections -MMD
CFLAGS    := $(COMMON) -std=gnu11
CXXFLAGS  := $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti -felide-constructors -Wno-psabi
LDSCRIPT  := src/device/doom.ld
LDFLAGS   := $(CPUFLAGS) -Wl,--gc-sections,--defsym=__rtc_localtime=0,--no-warn-rwx-segments \
             -Wl,--wrap=_sbrk $(WRAPS) -T$(LDSCRIPT) --specs=nano.specs
LIBS      := -lm -lstdc++

# OpenTracker: Teensy core, entry point, HAL, FatFs, and the few core files the
# HAL calls into (CPU meter cycle source, CRC-32, USB mass storage, UI fonts).
OT_C      := $(wildcard $(CORE)/*.c) $(wildcard $(OT)/src/hal/*.c) \
             $(OT)/third_party/fatfs/ff.c $(OT)/third_party/fatfs/ffunicode.c
OT_CXX    := $(filter-out $(CORE)/main.cpp,$(wildcard $(CORE)/*.cpp)) $(OT)/src/main.cpp \
             $(wildcard $(OT)/src/hal/*.cpp) $(OT)/src/core/audio/cpu_meter.cpp \
             $(OT)/src/core/formats/crc32.cpp $(OT)/src/core/usb/msc_bot.cpp $(OT)/src/core/ui/fonts/fonts.cpp
DEV_CXX   := $(wildcard src/device/*.cpp)

DEV_OBJS  := $(patsubst %,$(BUILD)/dev/%.o,$(OT_C) $(OT_CXX) $(DEV_CXX) $(DOOM_SRCS) $(GLUE_SRCS))

# Speed where Doom spends its frame; size elsewhere (as OpenTracker: HAL -Os,
# except the display / SD / audio ISR paths).
O2_SRCS   := $(DG)/r_draw.c $(DG)/r_plane.c $(DG)/r_segs.c $(DG)/r_bsp.c $(DG)/r_main.c $(DG)/r_things.c \
             $(DG)/m_fixed.c $(DG)/p_maputl.c $(DG)/p_map.c $(DG)/p_sight.c $(DG)/p_mobj.c $(DG)/p_tick.c \
             $(DG)/p_enemy.c $(DG)/v_video.c $(DG)/z_zone.c src/doom/pt_sound.c $(CORE)/usb_dev.c \
             $(OT)/src/hal/ft81x.cpp $(OT)/src/hal/sdhc.cpp $(OT)/src/hal/sdram_cache.cpp \
             $(OT)/src/hal/audio_sgtl5000.cpp $(OT)/src/hal/diag.cpp $(OT)/src/hal/board.cpp \
             $(OT)/src/hal/usb_storage.cpp
$(patsubst %,$(BUILD)/dev/%.o,$(O2_SRCS)): OPT := -O2
OPT       ?= -Os
$(patsubst %,$(BUILD)/dev/%.o,$(DOOM_SRCS) $(GLUE_SRCS)): XFLAGS := $(DOOM_DEFS) $(DOOM_INCS)
$(patsubst %,$(BUILD)/dev/%.o,$(DOOM_SRCS)): WARN := $(DOOM_WARN)
$(patsubst %,$(BUILD)/dev/%.o,$(OT_C) $(OT_CXX) $(DEV_CXX)): XFLAGS := $(OT_INCS)
$(patsubst %,$(BUILD)/dev/%.o,$(DEV_CXX)): XFLAGS += $(DOOM_INCS) -Wall

all: $(PTF)

$(BUILD)/dev/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OPT) $(XFLAGS) $(WARN) -c $< -o $@

$(BUILD)/dev/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OPT) $(XFLAGS) $(WARN) -c $< -o $@

$(BUILD)/doom.elf: $(DEV_OBJS) $(LDSCRIPT)
	$(CXX) $(LDFLAGS) $(DEV_OBJS) -L$(CORE) $(LIBS) -o $@ -Wl,-Map=$(BUILD)/doom.map
	$(SIZE) $@

$(BUILD)/doom.hex: $(BUILD)/doom.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

# The stock updater loads Intel HEX files named *.ptf from /Firmware.
$(PTF): $(BUILD)/doom.hex
	python3 -I tools/size_report.py --size-tool $(SIZE) $(BUILD)/doom.elf
	cp $< $@

# ---- host ---------------------------------------------------------------------------
HOST_CC   ?= gcc
HOST_SRCS := $(DOOM_SRCS) $(GLUE_SRCS) host/host_main.c
HOST_OBJS := $(patsubst %,$(BUILD)/host/%.o,$(HOST_SRCS))
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

$(BUILD)/host/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -g -MMD $(DOOM_DEFS) $(DOOM_INCS) $(SDL_CFLAGS) $(if $(filter $(DG)/%,$<),-w,-Wall -Wextra) -c $< -o $@

$(BUILD)/host/doom-host: $(HOST_OBJS)
	$(HOST_CC) $(HOST_OBJS) $(WRAPS) $(SDL_LIBS) -lm -o $@

host: $(BUILD)/host/doom-host

# Headless scripted runs (tools/host_check.py): demo, new game, keys, jog walking.
host-check: $(BUILD)/host/doom-host
	@test -f "$(WAD)" || { echo "host-check: no WAD at $(WAD) (WAD=...)"; exit 1; }
	cd $(BUILD)/host && python3 -I $(abspath tools/host_check.py) ./doom-host "$(abspath $(WAD))" $(abspath tests/scripts)

# ---- host unit tests ---------------------------------------------------------------
TEST_CFLAGS := -O1 -g -Wall -Wextra $(DOOM_DEFS) $(DOOM_INCS) -fsanitize=address,undefined

$(BUILD)/test/mus_test: tests/mus_test.c src/doom/pt_music.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(TEST_CFLAGS) $^ -o $@

$(BUILD)/test/mix_test: tests/mix_test.c src/doom/pt_sound.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(TEST_CFLAGS) $^ -o $@

test: $(BUILD)/test/mus_test $(BUILD)/test/mix_test
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test/mix_test
	@test -f "$(WAD)" || { echo "test: no WAD at $(WAD) (WAD=...)"; exit 1; }
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test/mus_test "$(WAD)"

clean:
	rm -rf $(BUILD)

-include $(DEV_OBJS:.o=.d) $(HOST_OBJS:.o=.d)
.PHONY: all host host-check test clean
