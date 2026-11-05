# SPDX-License-Identifier: MIT
#
# Host tests for infusion-sim (implementation_plan_infusion_pump.md, phase 3):
# the model of the mechanism, the producer's side of the actuator protocol -
# the dead-man timer above all - and the two together behind real sockets,
# driven by the framework's own MedicalActuator and MedicalDevice drivers.
#
# Compiled from the recipe's sources and the framework's, never copied, with
# AddressSanitizer and UBSan on. The framework's runner is reused.

CXX       ?= g++
APP       := ../../meta-custom/meta-med-app/recipes-apps/infusion-sim/files/src
FRAMEWORK := ../../meta-custom/meta-med-framework/recipes-framework/med-framework-api/files
AMP_ABI   := ../../meta-custom/meta-med-framework/recipes-framework/med-amp-abi/files

BUILD := build
BIN   := $(BUILD)/infusion-sim-tests

CXXFLAGS ?= -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter -Werror
CXXFLAGS += -I$(BUILD)/include -I$(FRAMEWORK)/include -I$(AMP_ABI) -I$(APP) -I../framework
LDLIBS   += $(shell pkg-config --libs libcrypto 2>/dev/null) -lpthread

SOURCES := main.cpp \
	$(APP)/syringe_model.cpp \
	$(APP)/mechanism.cpp \
	$(APP)/simulator.cpp \
	$(FRAMEWORK)/src/med-framework.cpp \
	$(FRAMEWORK)/src/MedicalDevice.cpp \
	$(FRAMEWORK)/src/MedicalActuator.cpp \
	$(FRAMEWORK)/src/MedicalIPC.cpp \
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
