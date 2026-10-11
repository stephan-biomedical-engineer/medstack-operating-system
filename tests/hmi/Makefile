# SPDX-License-Identifier: MIT
#
# Host tests for the operator display's arithmetic (TraceStore.cpp): the sweep
# buffer, the reduction of samples to screen columns, the display filters and
# the delivered-rate meter (implementation_plan_hmi_desktop.md, phase A2).
#
# The source is NOT copied. It is compiled from the recipe that ships it, and it
# includes no toolkit header, which is what lets a display's logic be tested on
# a host with no display. What draws - the scene graph item and the QML - is
# not reachable from here; the board is the evidence for that half.

CXX   ?= g++
HMI   := ../../meta-custom/meta-med-app/recipes-apps/eeg-hmi-gui/files/src

BUILD := build
BIN   := $(BUILD)/hmi-tests

CXXFLAGS ?= -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CXXFLAGS += -I$(HMI) -I../framework

SOURCES := main.cpp $(HMI)/TraceStore.cpp

.PHONY: all check clean
all: $(BIN)

check: $(BIN)
	@ASAN_OPTIONS=detect_leaks=0 $(BIN)

$(BIN): $(SOURCES) $(HMI)/TraceStore.h ../framework/check.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

clean:
	@rm -rf $(BUILD)
