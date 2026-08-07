# MedOS kernel policy, applied to whatever kernel the BSP happens to provide.
#
# The wildcard is deliberate (see CLAUDE.md rule 3): linux-yocto on QEMU,
# linux-st on the STM32MP257 and linux-raspberrypi on a Pi must all end up with
# the same distro level kernel capabilities, and the app/framework layers must
# never have to know which one is in use. Porting to a new BSP therefore costs
# zero lines here, which is exactly what the portability metric measures.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# "linux-%" also matches linux-libc-headers, linux-firmware and friends, which
# are not kernels and would fail on an unexpected SRC_URI entry. Only extend
# recipes that actually inherit the kernel class.
python __anonymous() {
    if bb.data.inherits_class('kernel', d):
        d.appendVar('SRC_URI', ' file://med-kernel-features.cfg')
}
