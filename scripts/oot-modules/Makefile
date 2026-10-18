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
# with them. By default both come from the working tree; MCP2210_REF and
# ADS1299_REF take each driver from a commit instead, because the two are
# independent and live on different branches:
#
#   make -C scripts/oot-modules MCP2210_REF=board/medplatform-afe \
#                               ADS1299_REF=feature/ads1299-mcp2210-driver
#
# Measured 2026-09-29 against 6.8.0-142-generic (drivers written for 6.6): both
# compile with no warnings, modpost resolves hid, counter, industrialio and
# kfifo_buf. Nothing was loaded; with Secure Boot on, insmod refuses an unsigned
# module. Compiling proves API compatibility, not behaviour.
#
# The feature ti-ads1299 raises IIO_EV_TYPE_FAULT / IIO_EV_DIR_FAULT_OPENWIRE,
# which mainline added after 6.8 (this repo's 6.6 tree has it backported).
# Against headers without it the copy is built with no event spec: the two
# lines that attach it to the channels are removed, so the event cannot be
# enabled and is never pushed. Defining the two names alone would not do: the
# core names an event's sysfs file from a table indexed by its type, and a
# type the core does not know reads past the end of that table.

ROOT := $(abspath $(CURDIR)/../..)
OUT  := $(ROOT)/build/oot-modules
KDIR ?= /lib/modules/$(shell uname -r)/build
LINUX := $(ROOT)/linux-med

MCP2210_REF ?=
ADS1299_REF ?=

# $(call src,REF,PATH,DEST): the file from REF, or from the working tree
src = $(if $(1),git -C $(LINUX) show $(1):$(2) > $(3),cp $(LINUX)/$(2) $(3))

HAS_FAULT_EVENT := $(shell grep -q IIO_EV_TYPE_FAULT \
	$(KDIR)/include/uapi/linux/iio/types.h && echo y)

all: $(OUT)/Kbuild
	$(MAKE) -C $(KDIR) M=$(OUT) modules

# In-tree these come from Kconfig; out of tree the symbol has to be said.
# Always rebuilt: the sources can come from a branch, and a branch moves.
$(OUT)/Kbuild: FORCE
	@mkdir -p $(OUT)
	$(call src,$(MCP2210_REF),drivers/hid/hid-mcp2210.c,$(OUT)/hid-mcp2210.c)
	$(call src,$(MCP2210_REF),drivers/hid/hid-ids.h,$(OUT)/hid-ids.h)
	$(call src,$(ADS1299_REF),drivers/iio/adc/ti-ads1299.c,$(OUT)/ti-ads1299.c)
	printf '%s\n' 'obj-m += hid-mcp2210.o ti-ads1299.o' \
	  'ccflags-y += -DCONFIG_HID_MCP2210_MODULE=1 -DCONFIG_TI_ADS1299_MODULE=1' > $@
ifneq ($(HAS_FAULT_EVENT),y)
	@if grep -q IIO_EV_TYPE_FAULT $(OUT)/ti-ads1299.c; then \
	  sed -i -e '/\.event_spec = ads1299_events,/d' \
	         -e '/\.num_event_specs = ARRAY_SIZE(ads1299_events),/d' \
	         $(OUT)/ti-ads1299.c; \
	  if grep -q 'event_spec = ads1299_events\|num_event_specs' $(OUT)/ti-ads1299.c; then \
	    echo 'oot-modules: the event spec moved, update the compat step' >&2; exit 1; fi; \
	  printf '%s\n' 'CFLAGS_ti-ads1299.o += -DIIO_EV_TYPE_FAULT=-1 -DIIO_EV_DIR_FAULT_OPENWIRE=-1' >> $@; \
	  echo 'oot-modules: $(KDIR) has no IIO_EV_TYPE_FAULT; ti-ads1299 built without the lead-off event'; \
	fi
endif

clean:
	rm -rf $(OUT)

.PHONY: all clean FORCE
