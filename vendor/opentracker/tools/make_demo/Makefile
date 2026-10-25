# Host tool: writes a ready-to-play demo project onto an SD-card folder using
# the App's own project/instrument save paths (src/core, unchanged).
#   make -C reimpl/tools/make_demo   ->  reimpl/tools/make_demo/make_demo
#   reimpl/tools/make_demo/make_demo <sd-root>
CXX      ?= g++
ROOT     := ../..
OUT      := $(ROOT)/build/make_demo
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-format-truncation -MMD \
            -I$(ROOT)/src/core -I$(ROOT)/src -I$(ROOT)/sim/src

CORE_SRCS := $(shell find $(ROOT)/src/core -name '*.cpp')
CORE_OBJS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/obj/%.o,$(CORE_SRCS))
TOOL_OBJS := $(OUT)/obj/make_demo.o $(OUT)/obj/host_storage.o
BIN       := make_demo

all: $(BIN)

$(OUT)/obj/%.o: $(ROOT)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/make_demo.o: make_demo.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/host_storage.o: $(ROOT)/sim/src/host_storage.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(CORE_OBJS) $(TOOL_OBJS)
	$(CXX) $^ -lpthread -o $@

clean:
	rm -rf $(OUT) $(BIN)

-include $(CORE_OBJS:.o=.d) $(TOOL_OBJS:.o=.d)
.PHONY: all clean
