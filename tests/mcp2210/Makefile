# SPDX-License-Identifier: MIT
#
# Host tests for the MCP2210 bridge driver. No bitbake, no image, no target,
# no cross toolchain - the driver source is compiled for the host against a
# shim that supplies the kernel APIs it calls.
#
# The driver is NOT copied here. It is included from the kernel tree that is
# its source of truth, so a suite that passes is a suite that ran against the
# code that ships.

CC      ?= cc
DRIVER  := ../../linux-med/drivers/hid/hid-mcp2210.c

# -I. so that the driver's <linux/...> includes resolve to the stubs here.
# -Wno-unused-function because a shim defines more of the kernel than any one
# driver calls, and that is the point of a shim.
#
# AddressSanitizer is on by default and is not optional decoration: struct
# mcp2210 holds rx_flat immediately before the cached chip settings, so the one
# memory defect this driver can have - a device claiming more received bytes
# than it sent - writes past an array into its own neighbour. Without ASan that
# shows up as "free(): invalid pointer" from glibc at some later, unrelated
# point; with it, it is named at the instruction that did it. Leak detection is
# off because the shim deliberately never frees what devres would have.
CFLAGS  ?= -std=gnu11 -g -O1 -I. \
           -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
           -fno-strict-aliasing -fsanitize=address

BUILD   := build
BIN     := $(BUILD)/mcp2210-tests
OBJS    := $(BUILD)/main.o $(BUILD)/fake_mcp2210.o

.PHONY: check clean
check: $(BIN)
	@ASAN_OPTIONS=detect_leaks=0 $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/main.o: main.c check.h fake_mcp2210.h kernel_shim.h kernel_shim_dev.h $(DRIVER)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ main.c

$(BUILD)/fake_mcp2210.o: fake_mcp2210.c fake_mcp2210.h kernel_shim.h kernel_shim_dev.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ fake_mcp2210.c

clean:
	rm -rf $(BUILD)
