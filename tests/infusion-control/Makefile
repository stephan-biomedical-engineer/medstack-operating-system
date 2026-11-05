# SPDX-License-Identifier: MIT
#
# Host tests for infusion-control-service (implementation_plan_infusion_pump.md,
# phase 4): every transition of the controller, the forbidden ones included,
# the dose error reduction, the alarms and the supervision of the mechanism,
# with time passed in; and the sealed configuration's refusals.
#
# Compiled from the recipe's sources and the framework's, never copied, with
# AddressSanitizer and UBSan on. The framework's runner is reused.

CXX       ?= g++
APP       := ../../meta-custom/meta-med-app/recipes-apps/infusion-control-service/files/src
FRAMEWORK := ../../meta-custom/meta-med-framework/recipes-framework/med-framework-api/files
AMP_ABI   := ../../meta-custom/meta-med-framework/recipes-framework/med-amp-abi/files

BUILD := build
BIN   := $(BUILD)/infusion-control-tests

CXXFLAGS ?= -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter -Werror
CXXFLAGS += -DINFUSION_CONF='"$(abspath $(APP)/../infusion.conf)"'
CXXFLAGS += -I$(BUILD)/include -I$(FRAMEWORK)/include -I$(AMP_ABI) -I$(APP) -I../framework
LDLIBS   += $(shell pkg-config --libs libcrypto 2>/dev/null) -lpthread

SOURCES := main.cpp \
	$(APP)/controller.cpp \
	$(APP)/library.cpp \
	$(FRAMEWORK)/src/med-framework.cpp \
	$(FRAMEWORK)/src/MedicalConfiguration.cpp

.PHONY: all check clean
all: $(BIN)

check: $(BIN)
	@ASAN_OPTIONS=detect_leaks=0 $(BIN)

$(BUILD)/include/medplatform:
	@mkdir -p $(BUILD)/include
	@ln -sfn $(abspath $(FRAMEWORK)/include) $@

$(BIN): $(SOURCES) $(wildcard $(APP)/*.h) $(wildcard $(FRAMEWORK)/include/*.h) | $(BUILD)/include/medplatform
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	@rm -rf $(BUILD)
