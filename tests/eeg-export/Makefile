# SPDX-License-Identifier: MIT
#
# Host tests for eeg-record-export's core (record_export.cpp): a session
# record in, BDF+ files out, and every way a record can be damaged in between
# (implementation_plan_bdf.md, phase 3).
#
# The converter is NOT copied. It is compiled from the recipe that ships it,
# against the framework sources that recipe links, so a suite that passes ran
# against the code on the device. AddressSanitizer and UBSan are on: the
# converter walks a binary record by offsets, which is where an off-by-one
# hides. The framework's own runner and scratch directory are reused.

CXX       ?= g++
APP       := ../../meta-custom/meta-med-app/recipes-apps/eeg-acquisition-service/files/src
FRAMEWORK := ../../meta-custom/meta-med-framework/recipes-framework/med-framework-api/files
AMP_ABI   := ../../meta-custom/meta-med-framework/recipes-framework/med-amp-abi/files

BUILD := build
BIN   := $(BUILD)/eeg-export-tests

CXXFLAGS ?= -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter -Werror
# The application includes <medplatform/...>, as it does against the installed
# library; $(BUILD)/include/medplatform points at the framework's headers.
CXXFLAGS += -I$(BUILD)/include -I$(FRAMEWORK)/include -I$(AMP_ABI) -I../framework
LDLIBS   += $(shell pkg-config --libs libcrypto 2>/dev/null) -lpthread

SOURCES := main.cpp \
	$(APP)/record_export.cpp \
	$(FRAMEWORK)/src/med-framework.cpp \
	$(FRAMEWORK)/src/MedicalStorage.cpp \
	$(FRAMEWORK)/src/MedicalStorageBiosignal.cpp \
	$(FRAMEWORK)/src/MedicalDevice.cpp \
	$(FRAMEWORK)/src/MedicalIPC.cpp \
	$(FRAMEWORK)/src/MedicalConfiguration.cpp

.PHONY: all check clean
all: $(BIN)

check: $(BIN)
	@ASAN_OPTIONS=detect_leaks=0 $(BIN)

$(BUILD)/include/medplatform:
	@mkdir -p $(BUILD)/include
	@ln -sfn $(abspath $(FRAMEWORK)/include) $@

$(BIN): $(SOURCES) $(APP)/record_export.h $(wildcard $(FRAMEWORK)/include/*.h) | $(BUILD)/include/medplatform
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	@rm -rf $(BUILD)
