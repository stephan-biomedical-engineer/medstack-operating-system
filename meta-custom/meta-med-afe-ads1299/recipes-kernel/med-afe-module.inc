# Common shape of the two out-of-tree kernel modules in this adjunct layer.
#
# The interesting part is the configuration guard. Both modules need kernel
# symbols that neither of the two kernels this repository builds agrees on:
#
#   linux-yocto 6.6.144 (qemux86-64)   # CONFIG_SPI is not set
#                                      # CONFIG_GPIOLIB is not set
#                                      # CONFIG_IIO_KFIFO_BUF is not set
#   linux-stm32mp 6.6.129 (stm32mp2)   CONFIG_SPI=y, CONFIG_GPIOLIB=y,
#                                      CONFIG_IIO_KFIFO_BUF=y
#
# That asymmetry is the same defect shape as CONFIG_DM_VERITY, which this
# repository has already paid for once: a capability the platform assumes,
# never writes down, and gets for free from one vendor's defconfig while the
# other silently lacks it. med-kernel-features.cfg now declares all of them,
# and this guard is what makes the declaration checkable - because a fragment
# whose symbol nothing selects is applied and lost without a word (merge_config
# reports it, in a log nobody reads).
#
# Failing here costs seconds. The alternative failure is a module that builds
# against a kernel missing a symbol and then does not load on the board, which
# costs a bench session - and BRINGUP_STM32MP2.md §11 already records what that
# is worth.

inherit module

# The source tree lives in a subdirectory rather than directly in WORKDIR, for
# the reason med-framework-api_1.0.0.bb records: S = ${WORKDIR} puts PKGD
# inside S, and the debug-source hardlinks of do_package then make pseudo abort
# on any task re-run.
S = "${WORKDIR}/sources"

MED_AFE_KCONFIG_REQUIRED ?= ""

python do_check_kernel_config() {
    import os

    required = (d.getVar('MED_AFE_KCONFIG_REQUIRED') or '').split()
    if not required:
        return

    config = os.path.join(d.getVar('STAGING_KERNEL_BUILDDIR'), '.config')
    if not os.path.exists(config):
        bb.fatal("no kernel .config at %s - cannot verify what this module "
                 "needs from the kernel" % config)

    with open(config) as handle:
        text = handle.read()

    missing = [sym for sym in required
               if ('%s=y' % sym) not in text and ('%s=m' % sym) not in text]

    if missing:
        bb.fatal(
            "the kernel built for MACHINE=%s does not provide %s.\n"
            "These are declared in meta-med-distro's med-kernel-features.cfg. "
            "A symbol with no prompt (CONFIG_REGMAP is one) cannot be turned "
            "on by a fragment at all - it has to be selected by something the "
            "kernel builds - so this is a real gap and not a typo in the "
            "fragment." % (d.getVar('MACHINE'), ' '.join(missing)))
}
addtask check_kernel_config before do_configure
do_check_kernel_config[depends] += "make-mod-scripts:do_compile"
