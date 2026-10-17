# Host tests of the converter's kernel driver, linux-med/drivers/iio/adc/ti-ads1299.c
# (docs/implementation_plan_usb_parity.md, phase 0.a). Runs in seconds, with no
# bitbake, no image, no board and no cross toolchain:
#
#   make -C tests/ads1299 check     (or `make test` at the root)
#
# The driver is compiled as it ships, included by main.c, against the shim in
# this directory; the M33 firmware's register builder (afe_regs.c) is compiled
# in as the oracle for the bytes both drivers write to the same part.

CC      ?= cc
DRIVER  := ../../linux-med/drivers/iio/adc/ti-ads1299.c
M33     := ../../meta-custom/meta-med-afe-ads1299/dynamic-layers/stm-st-stm32mp/recipes-firmware/med-m33-firmware/files/src
CFLAGS  ?= -std=gnu11 -g -O1 -I. -I$(M33) \
           -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
           -Wno-missing-field-initializers -fno-strict-aliasing -fsanitize=address,undefined
BUILD   := build
BIN     := $(BUILD)/ads1299-tests
# The branch going upstream has its own binary: feature.c tests what main.c
# cannot compile. Its driver comes from the branch itself, so no checkout is
# needed; FEATURE_DRIVER=<file> tests a working copy instead.
FEATURE_REF    ?= feature/ads1299-mcp2210-driver
FEATURE_DRIVER ?= $(BUILD)/feature-ti-ads1299.c
FEATURE_BIN    := $(BUILD)/ads1299-feature-tests
OBJS    := $(BUILD)/main.o $(BUILD)/fake_ads1299.o $(BUILD)/kernel_shim.o $(BUILD)/afe_regs.o
HEADERS := kernel_shim.h fake_ads1299.h shim_test.h check.h

.PHONY: check clean FORCE
check: $(BIN) $(FEATURE_BIN)
	@echo "linux-med em $$(git -C ../../linux-med branch --show-current 2>/dev/null) ($$(git -C ../../linux-med rev-parse --short HEAD 2>/dev/null))"
	@ASAN_OPTIONS=detect_leaks=0 $(BIN)
	@echo
	@echo "driver do PR: $(FEATURE_DRIVER)"
	@ASAN_OPTIONS=detect_leaks=0 $(FEATURE_BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lm

$(BUILD)/main.o: main.c $(HEADERS) $(DRIVER) $(M33)/afe_regs.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ main.c

$(BUILD)/feature-ti-ads1299.c: FORCE
	@mkdir -p $(BUILD)
	@git -C ../../linux-med show $(FEATURE_REF):drivers/iio/adc/ti-ads1299.c > $@.new
	@cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

# Always rebuilt: FEATURE_DRIVER can name an older file than the last one, and
# a date comparison would then keep the binary of the other driver.
$(BUILD)/feature.o: feature.c $(HEADERS) $(FEATURE_DRIVER) FORCE
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DFEATURE_DRIVER='"$(abspath $(FEATURE_DRIVER))"' -c -o $@ feature.c

$(FEATURE_BIN): $(BUILD)/feature.o $(BUILD)/fake_ads1299.o $(BUILD)/kernel_shim.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/afe_regs.o: $(M33)/afe_regs.c $(M33)/afe_regs.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build
