# Host tool: writes the OpenTracker synth preset pack (PRESETS.md) with the
# App's own Save As path (src/core, unchanged).
#   make -C reimpl/tools/make_patches           -> build/make_patches/make_patches
#   make -C reimpl/tools/make_patches run       -> build/patches/Patches/<MODEL>/<name>.patch
#   make -C reimpl/tools/make_patches markdown  -> regenerates PRESETS.md
#   make -C reimpl patches                      (same as run)
CXX      ?= g++
ROOT     := ../..
OUT      := $(ROOT)/build/make_patches
SD       ?= $(ROOT)/build/patches
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wno-format-truncation -MMD \
            -I$(ROOT)/src/core -I$(ROOT)/src -I$(ROOT)/sim/src

CORE_SRCS := $(shell find $(ROOT)/src/core -name '*.cpp')
CORE_OBJS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/obj/%.o,$(CORE_SRCS))
TOOL_OBJS := $(OUT)/obj/tool/main.o $(OUT)/obj/tool/presets.o $(OUT)/obj/host_storage.o
BIN       := $(OUT)/make_patches

all: $(BIN)

$(OUT)/obj/%.o: $(ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/tool/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/host_storage.o: $(ROOT)/sim/src/host_storage.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(CORE_OBJS) $(TOOL_OBJS)
	$(CXX) $^ -lpthread -o $@

# Fresh card folder each run; only Patches/ is meant for the card (the
# generator's own Workspace, config and project folders are removed).
run: $(BIN)
	rm -rf $(SD)
	mkdir -p $(SD)
	$(BIN) $(SD)
	cd $(SD) && find . -mindepth 1 -maxdepth 1 ! -name Patches -exec rm -rf {} +

markdown: $(BIN)
	$(BIN) --markdown > PRESETS.md

clean:
	rm -rf $(OUT)

-include $(CORE_OBJS:.o=.d) $(TOOL_OBJS:.o=.d)
.PHONY: all run markdown clean
