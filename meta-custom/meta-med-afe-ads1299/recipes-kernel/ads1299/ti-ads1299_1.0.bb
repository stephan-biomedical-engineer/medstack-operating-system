SUMMARY = "IIO driver for the TI ADS1299 biopotential front-end"
DESCRIPTION = "Out-of-tree IIO driver for the ADS1299, the 8-channel 24-bit \
EEG member of the TI ADS129x family. Derived from drivers/iio/adc/ti-ads1298.c \
(Topic Embedded Products, mainline since Linux 6.9) and backported to the 6.6 \
kernels this platform builds. Because it is a derivative of reviewed mainline \
code rather than an adopted unmaintained project, it is a software item under \
this project's own configuration control and not SOUP (IEC 62304 §3.29)."

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

require recipes-kernel/med-afe-module.inc

SRC_URI = " \
    file://Makefile;subdir=sources \
    file://ti-ads1299.c;subdir=sources \
"

# CONFIG_IIO_KFIFO_BUF is the one that catches people: IIO and IIO_BUFFER can
# both be set while devm_iio_kfifo_buffer_setup() is not available, and the
# failure is an undefined symbol at modpost, two steps from the cause.
# CONFIG_REGMAP has no Kconfig prompt - it is selected by whatever needs it -
# so it is listed here as something to *detect*, not something a fragment can
# turn on.
MED_AFE_KCONFIG_REQUIRED = " \
    CONFIG_SPI \
    CONFIG_IIO \
    CONFIG_IIO_BUFFER \
    CONFIG_IIO_KFIFO_BUF \
    CONFIG_REGMAP \
"

# Not autoloaded. Unlike the bridge, this driver binds through the SPI bus -
# either from a devicetree node (hat link) or because mcp2210-spi created the
# device (USB link) - and in both cases the module is pulled in by modalias at
# the moment a device appears. Loading it eagerly on a machine whose converter
# lives on the Cortex-M33 would put an unused SPI driver in a product image.
