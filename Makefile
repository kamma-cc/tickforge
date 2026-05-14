CXX      ?= g++
CXXSTD   ?= -std=c++20
OPT      ?= -O3
WARN     ?= -Wall -Wextra -Wpedantic
CXXFLAGS ?= $(CXXSTD) $(OPT) $(WARN) -pthread
INCLUDES := -Iinclude
LDFLAGS  := -lrt -lpthread

BUILD := build
BIN   := $(BUILD)/bin
OBJ   := $(BUILD)/obj

LIB_SRCS := src/shm_ring_sink.cpp src/generator.cpp src/replayer.cpp
LIB_OBJS := $(patsubst src/%.cpp,$(OBJ)/%.o,$(LIB_SRCS))

TOOLS := tf_generate tf_replay
DEMOS := inprocess_demo shm_consumer

ALL_BINS := $(addprefix $(BIN)/,$(TOOLS) $(DEMOS))

.PHONY: all clean tools demos
all: $(ALL_BINS)
tools: $(addprefix $(BIN)/,$(TOOLS))
demos: $(addprefix $(BIN)/,$(DEMOS))

$(OBJ)/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BIN)/tf_generate: tools/tf_generate.cpp $(LIB_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BIN)/tf_replay: tools/tf_replay.cpp $(LIB_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BIN)/inprocess_demo: examples/inprocess_demo.cpp $(LIB_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(LIB_OBJS) $(LDFLAGS) -o $@

$(BIN)/shm_consumer: examples/shm_consumer.cpp $(LIB_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(LIB_OBJS) $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD)
