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

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -g -O0
# The same warning set the library is built with, plus the ones that matter for
# test code. -Werror is on: a warning in a test suite is a test suite nobody
# reads the output of.
CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter -Werror
CXXFLAGS += -I$(FRAMEWORK)/include -I.

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
	test_device.cpp

ifeq ($(HAVE_SYSTEMD),yes)
LIB_SOURCES  += $(FRAMEWORK)/src/MedicalLogger.cpp $(FRAMEWORK)/src/MedicalUpdate.cpp
TEST_SOURCES += test_logger.cpp test_update.cpp
endif

BUILD   := build
OBJECTS := $(addprefix $(BUILD)/,$(notdir $(LIB_SOURCES:.cpp=.o))) \
           $(addprefix $(BUILD)/,$(TEST_SOURCES:.cpp=.o))
BINARY  := $(BUILD)/med-framework-tests

.PHONY: all check clean
all: $(BINARY)

check: $(BINARY)
	@$(BINARY)

$(BINARY): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: $(FRAMEWORK)/src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
