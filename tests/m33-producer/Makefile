# SPDX-License-Identifier: MIT
#
# Host tests for the Cortex-M33 producer's logic (med_producer.c): the answer
# to a prescription, the frames, the synthetic signal. No board, no cross
# toolchain, no OpenAMP - main.c owns all of that and is not compiled here.
#
# The producer is NOT copied. It is compiled from the recipe that ships it,
# against the med_amp_abi.h the recipe builds against, so a suite that passes
# ran against the code on the M33. AddressSanitizer and UBSan are on: the
# producer memcpy()s fixed-size wire structs in and out of buffers, which is
# exactly where an off-by-one hides.

CC       ?= cc
PRODUCER := ../../meta-custom/meta-med-afe-ads1299/dynamic-layers/stm-st-stm32mp/recipes-firmware/med-m33-firmware/files/src
AMP_ABI  := ../../meta-custom/meta-med-framework/recipes-framework/med-amp-abi/files

CFLAGS   ?= -std=gnu11 -g -O1 -Wall -Wextra -Wconversion -Werror \
            -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS   += -I$(PRODUCER) -I$(AMP_ABI)
LDLIBS   += -lm

BUILD := build
BIN   := $(BUILD)/m33-producer-tests

.PHONY: all check clean
all: $(BIN)

check: $(BIN)
	@ASAN_OPTIONS=detect_leaks=0 $(BIN)

$(BIN): main.c $(PRODUCER)/med_producer.c $(PRODUCER)/med_producer.h $(PRODUCER)/afe_regs.c $(PRODUCER)/afe_regs.h $(AMP_ABI)/med_amp_abi.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ main.c $(PRODUCER)/med_producer.c $(PRODUCER)/afe_regs.c $(LDLIBS)

clean:
	@rm -rf $(BUILD)
