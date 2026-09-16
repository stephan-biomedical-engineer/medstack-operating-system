SUMMARY = "MCP2210 USB-to-SPI bridge as a Linux spi_controller"
DESCRIPTION = "Out-of-tree HID driver that registers the Microchip MCP2210 as \
a real spi_controller, plus its nine GPIOs and the GP6 edge counter. Written \
in this project rather than adopted: there is no MCP2210 driver in mainline, \
and the available out-of-tree one targets kernel 4.19, has been unmaintained \
since 2018 and carries an ioctl/configfs ABI this path does not need - which \
would make it SOUP in the kernel's trust domain. See \
docs/implementation_plan_iio_afe.md §2.2 and §3."

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

require recipes-kernel/med-afe-module.inc

SRC_URI = " \
    file://Makefile;subdir=sources \
    file://mcp2210-spi.c;subdir=sources \
"

MED_AFE_KCONFIG_REQUIRED = " \
    CONFIG_SPI \
    CONFIG_HID \
    CONFIG_USB_HID \
    CONFIG_GPIOLIB \
"

# Loaded at boot, and that is the fix for the one risk the plan named for this
# driver.
#
# hid_generic_match() declines any device that some other registered HID driver
# matches (drivers/hid/hid-generic.c:37-57 in the 6.6 tree), so winning the
# bind needs no quirk - but "registered" is the operative word. If the bridge
# enumerates before this module is loaded, hid-generic (built in, always
# present) binds it first, and nothing will hand it over afterwards without a
# manual unbind. Autoloading removes the race instead of documenting it.
KERNEL_MODULE_AUTOLOAD += "mcp2210-spi"

# The AFE driver is not a build dependency of this one - they are linked by the
# SPI bus at runtime, not by a symbol - but a bridge with nothing behind it is
# not a useful image. This is a runtime recommendation, so the two can still be
# installed separately for a bench session that brings the bridge up alone.
RRECOMMENDS:${PN} += "kernel-module-ti-ads1299"
