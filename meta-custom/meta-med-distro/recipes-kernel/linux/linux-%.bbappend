# MedOS kernel policy, applied to whatever kernel the BSP happens to provide.
#
# The wildcard is deliberate (see CLAUDE.md rule 3): linux-yocto on QEMU,
# linux-stm32mp on the STM32MP257 and linux-raspberrypi on a Pi must all end up
# with the same distro level kernel capabilities, and the app/framework layers
# must never have to know which one is in use. Porting to a new BSP therefore
# costs zero lines here, which is exactly what the portability metric measures.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# "linux-%" also matches linux-libc-headers, linux-firmware and friends, which
# are not kernels and would fail on an unexpected SRC_URI entry. Only extend
# recipes that actually inherit the kernel class.
#
# Two mechanisms, because "kernel recipe" is not one thing:
#
#   kernel-yocto (linux-yocto) scans SRC_URI for .cfg files by itself and feeds
#   them to its own configuration machinery. SRC_URI alone is enough there, and
#   that is why this bbappend worked on QEMU and looked portable.
#
#   plain kernel.bbclass (linux-stm32mp, and most vendor kernels) does not. ST's
#   linux-stm32mp.inc merges exactly the files listed in KERNEL_CONFIG_FRAGMENTS
#   with merge_config.sh and ignores everything else in SRC_URI. Appending to
#   SRC_URI alone therefore *unpacked* this fragment into the work directory and
#   never applied a line of it.
#
# That was not a hypothesis: KERNEL_CONFIG_FRAGMENTS for linux-stm32mp listed
# ST's four fragments and not this one, while SRC_URI listed this one - so every
# symbol MedOS asks for was, on that target, whatever ST's defconfig happened to
# say. The ones that came out right came out right by coincidence, which is the
# worst way for a security requirement to be satisfied. Appending last matters:
# merge_config.sh applies fragments in order and the last assignment wins.
python __anonymous() {
    if bb.data.inherits_class('kernel', d):
        d.appendVar('SRC_URI', ' file://med-kernel-features.cfg')
        d.appendVar('KERNEL_CONFIG_FRAGMENTS', ' ${WORKDIR}/med-kernel-features.cfg')
}
