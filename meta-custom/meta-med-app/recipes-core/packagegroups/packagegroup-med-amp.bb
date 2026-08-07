SUMMARY = "MedPlatform asymmetric multiprocessing (AMP) profile"
DESCRIPTION = "Capability marker and userspace enablement for device profiles \
whose acquisition path runs on a real-time co-processor (Cortex-M4 next to the \
Cortex-A35 on the STM32MP257), reached from Linux through MedicalIPC over \
OpenAMP rpmsg. \
\
The kernel side (REMOTEPROC, RPMSG_CHAR, virtio) is distro policy and comes \
from meta-med-distro's linux-%.bbappend, so it is already present on every \
target. What is genuinely machine specific is the co-processor firmware, and \
that belongs to the BSP: a BSP sets MED_AMP_FIRMWARE and it is pulled in here \
without this layer ever naming a machine."
LICENSE = "MIT"

PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup

# Set by the BSP or by the KAS project file for a machine that has a
# co-processor; empty on QEMU, where the "simulated" MedicalDevice driver
# stands in for the front-end.
MED_AMP_FIRMWARE ?= ""

RDEPENDS:${PN} = " \
    med-framework-api \
    ${MED_AMP_FIRMWARE} \
"
