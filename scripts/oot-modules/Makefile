# The two front-end drivers, built as out-of-tree modules against the kernel
# this host is running. NOT the product path: the Yocto build compiles them
# in-tree from ./linux-med (make stm32 KERNEL=med). This exists to answer "does
# it still compile against another kernel" and to load them on a machine with
# no board.
#
#   make -C scripts/oot-modules                 build for the running kernel
#   make -C scripts/oot-modules KDIR=/path      build for another kernel tree
#
# Sources are copied from ./linux-med into build/oot-modules, so the tree is
# never written to. hid-ids.h is a private header of drivers/hid and travels
# with them.
#
# Measured 2026-09-29 against 6.8.0-142-generic (drivers written for 6.6): both
# compile with no warnings, modpost resolves hid, counter, industrialio and
# kfifo_buf. Nothing was loaded; with Secure Boot on, insmod refuses an unsigned
# module. Compiling proves API compatibility, not behaviour.

ROOT := $(abspath $(CURDIR)/../..)
OUT  := $(ROOT)/build/oot-modules
KDIR ?= /lib/modules/$(shell uname -r)/build

all: $(OUT)/Kbuild
	$(MAKE) -C $(KDIR) M=$(OUT) modules

# In-tree these come from Kconfig; out of tree the symbol has to be said.
$(OUT)/Kbuild: $(ROOT)/linux-med/drivers/hid/hid-mcp2210.c \
               $(ROOT)/linux-med/drivers/iio/adc/ti-ads1299.c
	@mkdir -p $(OUT)
	cp $(ROOT)/linux-med/drivers/hid/hid-mcp2210.c \
	   $(ROOT)/linux-med/drivers/hid/hid-ids.h \
	   $(ROOT)/linux-med/drivers/iio/adc/ti-ads1299.c $(OUT)/
	printf '%s\n' 'obj-m += hid-mcp2210.o ti-ads1299.o' \
	  'ccflags-y += -DCONFIG_HID_MCP2210_MODULE=1 -DCONFIG_TI_ADS1299_MODULE=1' > $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
