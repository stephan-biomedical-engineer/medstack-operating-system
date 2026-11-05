SUMMARY = "IIO driver for the TI ADS1299 biopotential front-end, out of tree"
DESCRIPTION = "The converter's kernel driver, built from the out-of-tree \
repository of its upstream submission (ads1299-linux) by that repository's own \
Makefile: compat/configure.sh adapts a copy of the driver to what the kernel \
being built lacks, says what it changed, and the copy is compiled. Derived from \
mainline's drivers/iio/adc/ti-ads1298.c, so a software item under this \
project's configuration control and not SOUP (IEC 62304 §3.29). The same tree \
the PC station's medplatform-afe-dkms package builds."
HOMEPAGE = "https://github.com/stephan-biomedical-engineer/ads1299-linux"

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=eb723b61539feef013de476e68b5c50a"

# One source of truth for the driver, and it is this repository - what was
# validated end to end (a PC over USB, a Raspberry Pi over SPI) and what goes
# upstream. Until 2026-10-28 the board built a second copy, kept in linux-med's
# tree, which differed from it by the 6.6 API adaptations that compat/ now makes
# at build time; two copies of one driver are two drivers the day one of them
# gets a fix. The bridge (hid-mcp2210) stays in linux-med: it is board code.
#
# Pinned, never a branch head: the image says which driver it carries. Moving
# it is an edit here, and the commit that moves it is the record.
SRC_URI = "git://github.com/stephan-biomedical-engineer/ads1299-linux.git;protocol=https;branch=main"
SRCREV = "458d05df5b119c421143709b4d571e1cd42f9026"
PV = "0.1+git"
# The repository is tagged v0.1. The recipe it replaces (deleted in 27ba8ee)
# shipped a package of this name at 1.0, so the epoch moves instead of the
# version: what an image carried must not look newer than what replaces it.
PE = "1"

S = "${WORKDIR}/git"

inherit module

# The repository's Makefile takes the kernel as KDIR. module.bbclass passes the
# kernel's build directory as O=, on the command line, which reaches the
# sub-makes the Makefile and compat/configure.sh run (MAKEFLAGS) - and
# configure.sh finds the kernel source through kbuild's own srctree, which is
# what it was written to do for an O= build.
EXTRA_OEMAKE += "KDIR=${STAGING_KERNEL_DIR}"

# Kbuild runs in build/, where configure.sh puts the adapted copy.
MODULES_MODULE_SYMVERS_LOCATION = "build"

# What the driver needs from the kernel it is built against (its Kconfig entry
# in the submission: depends on SPI, COMMON_CLK, GPIOLIB; selects IIO_BUFFER,
# IIO_KFIFO_BUF, REGMAP). Built out of tree, nothing selects those for it: a
# symbol with no prompt (REGMAP, IIO_KFIFO_BUF) is present only if something
# else the kernel builds selects it, and absent it fails at modpost - or worse,
# at modprobe on the board. Checked here, in seconds, against the final .config
# (the guard the deleted out-of-tree recipes carried, 27ba8ee^).
MED_AFE_KCONFIG_REQUIRED = " \
    CONFIG_SPI \
    CONFIG_COMMON_CLK \
    CONFIG_GPIOLIB \
    CONFIG_IIO \
    CONFIG_IIO_BUFFER \
    CONFIG_IIO_KFIFO_BUF \
    CONFIG_REGMAP \
"

python do_check_kernel_config() {
    import os

    config = os.path.join(d.getVar('STAGING_KERNEL_BUILDDIR'), '.config')
    if not os.path.exists(config):
        bb.fatal("no kernel .config at %s - cannot verify what ti-ads1299 needs "
                 "from the kernel" % config)
    have = {}
    with open(config) as handle:
        for line in handle:
            line = line.strip()
            if line.startswith('CONFIG_') and '=' in line:
                key, value = line.split('=', 1)
                have[key] = value

    missing = [sym for sym in (d.getVar('MED_AFE_KCONFIG_REQUIRED') or '').split()
               if have.get(sym) not in ('y', 'm')]
    if missing:
        bb.fatal("the kernel built for MACHINE=%s does not provide %s, which "
                 "ti-ads1299 needs. A symbol with no Kconfig prompt (CONFIG_REGMAP, "
                 "CONFIG_IIO_KFIFO_BUF) cannot be turned on by a fragment - it is "
                 "selected by what the kernel builds - so this is a gap in the "
                 "kernel, not a typo." % (d.getVar('MACHINE'), ' '.join(missing)))

    # And the other copy must not be built. With the in-tree driver enabled the
    # kernel would package a kernel-module-ti-ads1299 of its own, two providers
    # of one module name, and which one an image gets would be bitbake's
    # choice. med-stm32mp-drivers.cfg says "is not set"; this is the check.
    if have.get('CONFIG_TI_ADS1299') in ('y', 'm'):
        bb.fatal("the kernel built for MACHINE=%s has CONFIG_TI_ADS1299=%s: the "
                 "in-tree copy of the converter driver is enabled, and this recipe "
                 "builds the driver from its out-of-tree repository. Only one of "
                 "them may exist." % (d.getVar('MACHINE'), have['CONFIG_TI_ADS1299']))
}
addtask check_kernel_config after do_patch before do_configure
do_check_kernel_config[depends] += "virtual/kernel:do_shared_workdir"

# Not autoloaded. It binds through the SPI bus - from a devicetree node on the
# "spi" link, or because the bridge created the device on "usb" - and is pulled
# in by modalias the moment a device appears.
