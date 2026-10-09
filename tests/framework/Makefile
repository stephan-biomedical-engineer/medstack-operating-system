# Host-side functional tests for the MedFramework.
#
# Deliberately not CMake and deliberately not part of the library's own
# CMakeLists: these tests compile the framework's sources for the HOST, with
# the host's libsystemd and libcrypto, and the library's build is the one
# bitbake drives for a target. Keeping them apart means running the evidence
# never needs a Yocto build, and a broken test can never break an image.
#
# Two dependencies, the same two the library has. If a third ever appears in
# this file, the claim the thesis makes about the framework has been quietly
# broken somewhere.

FRAMEWORK := ../../meta-custom/meta-med-framework/recipes-framework/med-framework-api/files
AMP_ABI   := ../../meta-custom/meta-med-framework/recipes-framework/med-amp-abi/files

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -g -O0
# The same warning set the library is built with, plus the ones that matter for
# test code. -Werror is on: a warning in a test suite is a test suite nobody
# reads the output of.
CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter -Werror
CXXFLAGS += -I$(FRAMEWORK)/include -I$(AMP_ABI) -I.

CRYPTO_LIBS := $(shell pkg-config --libs libcrypto 2>/dev/null)
LDLIBS      += $(CRYPTO_LIBS) -lpthread

# libsystemd is optional HERE and mandatory in the library. When its headers
# are missing, MedicalLogger is left out and the binary says so at the end of
# every run - an uncovered module has to be visible, because a suite that
# quietly tests less and still prints "0 falhas" is the exact failure this
# repository has already paid for once (22/22 on an image losing 62% of its
# samples).
HAVE_SYSTEMD := $(shell pkg-config --exists libsystemd && echo yes)

ifeq ($(HAVE_SYSTEMD),yes)
CXXFLAGS += $(shell pkg-config --cflags libsystemd) -DMED_TESTS_HAVE_SYSTEMD=1
LDLIBS   += $(shell pkg-config --libs libsystemd)
endif

# The library sources under test. MedicalUpdate is here only with libsystemd,
# and only HALF of it is tested: reportReady() talks to the service manager
# through NOTIFY_SOCKET, which a test can stand up with a datagram socket, and
# it is the half the A/B confirmation now rests on. The D-Bus half stays
# uncovered - a host without rauc.service on the bus can only be told "the
# daemon is not there", which tests the host and not the wrapper - and the
# binary says so on every run.
LIB_SOURCES := \
	$(FRAMEWORK)/src/med-framework.cpp \
	$(FRAMEWORK)/src/MedicalConfiguration.cpp \
	$(FRAMEWORK)/src/MedicalStorage.cpp \
	$(FRAMEWORK)/src/MedicalDevice.cpp \
	$(FRAMEWORK)/src/MedicalIPC.cpp

TEST_SOURCES := \
	main.cpp \
	test_types.cpp \
	test_configuration.cpp \
	test_storage.cpp \
	test_device.cpp \
	test_amp_abi.cpp

ifeq ($(HAVE_SYSTEMD),yes)
LIB_SOURCES  += $(FRAMEWORK)/src/MedicalLogger.cpp $(FRAMEWORK)/src/MedicalUpdate.cpp
TEST_SOURCES += test_logger.cpp test_update.cpp
endif

# The AMP wire format is a C header shared with the firmware on the real-time
# core (med_amp_abi.h), so part of this suite is C. amp_abi_c_probe.c is built
# three ways, and each answers a different question:
#
#   hosted C99, linked into the binary  - does a C compiler lay the structs out
#                                          as the C++ compiler does?
#   freestanding, -nostdinc              - does the header need a C library?
#                                          (it must not: the firmware has none)
#   arm-none-eabi-gcc, Cortex-M33        - does the toolchain that builds the
#                                          firmware pack it the same way? The
#                                          header's static assertions are the
#                                          test, so this one is "it compiles".
#
# Both C99 and C11 are compiled for the last two, because the header asserts
# through _Static_assert in one and through a typedef idiom in the other, and
# an assertion mechanism nobody compiled is an assertion nobody has.
CC     ?= cc
CFLAGS_ABI := -Wall -Wextra -Wpedantic -Wconversion -Werror -I$(AMP_ABI)
ABI_PROBE  := amp_abi_c_probe.c

# The firmware's compiler is the one bitbake stages for gcc-arm-none-eabi-native;
# a host install is accepted when no build has run. Neither present is not a
# failure, but it is printed, because it means the cross-toolchain question
# went unasked.
ARM_CC ?= $(firstword \
  $(wildcard ../../build/tmp-glibc/sysroots-components/x86_64/gcc-arm-none-eabi-native/usr/share/gcc-arm-none-eabi/bin/arm-none-eabi-gcc) \
  $(shell command -v arm-none-eabi-gcc 2>/dev/null))

BUILD   := build
OBJECTS := $(addprefix $(BUILD)/,$(notdir $(LIB_SOURCES:.cpp=.o))) \
           $(addprefix $(BUILD)/,$(TEST_SOURCES:.cpp=.o)) \
           $(BUILD)/amp_abi_c_probe.o
BINARY  := $(BUILD)/med-framework-tests

.PHONY: all check clean abi-freestanding abi-cortex-m33
all: $(BINARY)

check: abi-freestanding abi-cortex-m33 $(BINARY)
	@$(BINARY)

abi-freestanding: | $(BUILD)
	@for std in c99 c11; do \
	  $(CC) -std=$$std -ffreestanding -nostdinc \
	    -isystem "$$($(CC) -print-file-name=include)" \
	    $(CFLAGS_ABI) -c -o $(BUILD)/abi-freestanding-$$std.o $(ABI_PROBE) || exit 1; \
	done
	@echo "med_amp_abi.h: freestanding, -nostdinc, C99 e C11 - compila"

abi-cortex-m33: | $(BUILD)
ifeq ($(ARM_CC),)
	@echo "med_amp_abi.h: NAO compilado para o Cortex-M33 (sem arm-none-eabi-gcc neste host)"
else
	@for std in c99 c11; do \
	  $(ARM_CC) -std=$$std -mcpu=cortex-m33 -mthumb -ffreestanding -nostdinc \
	    -isystem "$$($(ARM_CC) -print-file-name=include)" \
	    $(CFLAGS_ABI) -c -o $(BUILD)/abi-m33-$$std.o $(ABI_PROBE) || exit 1; \
	done
	@echo "med_amp_abi.h: Cortex-M33, C99 e C11 - compila com $$($(ARM_CC) -dumpversion) ($(ARM_CC))"
endif

$(BINARY): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: $(FRAMEWORK)/src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/amp_abi_c_probe.o: $(ABI_PROBE) $(AMP_ABI)/med_amp_abi.h | $(BUILD)
	$(CC) -std=c99 $(CFLAGS_ABI) -c -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
