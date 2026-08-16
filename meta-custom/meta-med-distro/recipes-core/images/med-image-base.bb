SUMMARY = "MedOS base platform image"
DESCRIPTION = "The device-class agnostic half of every MedPlatform product: \
systemd, the A/B update stack, the encrypted data volume and the audit-grade \
journal, and nothing else. Device profiles (EEG, tomograph, ...) live in \
meta-med-app and are built by requiring this recipe and adding packagegroups \
on top - which is precisely the code reuse the thesis measures."
LICENSE = "MIT"

inherit core-image

# Immutable rootfs is the default, not an option: it is what makes the A/B
# update story sound (the running slot cannot drift from the slot that was
# validated) and what bounds the effect of a compromised application.
# Development profiles override this to "" - see med-image-test.bb.
MED_ROOTFS_FEATURES ?= "read-only-rootfs"

IMAGE_FEATURES += "${MED_ROOTFS_FEATURES}"

# No package manager in the field: the only supported mutation is a signed
# RAUC bundle. (package-management is absent from IMAGE_FEATURES by default;
# this makes the intent explicit and survives an inherited local.conf.)
IMAGE_FEATURES:remove = "package-management"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    ${CORE_IMAGE_EXTRA_INSTALL} \
"

MED_OS_INSTALL = " \
    rauc \
    med-data-volume \
    cryptsetup \
    util-linux-blkid \
    util-linux-lsblk \
    e2fsprogs-mke2fs \
    e2fsprogs-resize2fs \
    ca-certificates \
    dbus \
"

IMAGE_INSTALL:append = " ${MED_OS_INSTALL}"

# The tooling that turns this repository's platform claims into measurements
# rather than assertions:
#   systemd-analyze     - "systemd-analyze security <unit>" scores the sandbox
#                         of each service, which is the evidence for the
#                         IEC 62304 5.3 software-partitioning argument.
#   util-linux-chrt     - confirms the acquisition service actually got the
#                         SCHED_RR priority its unit asks for; busybox has no
#                         chrt, and the image installs only util-linux-blkid
#                         and util-linux-lsblk, not the full util-linux.
#   util-linux-taskset  - CPU affinity, for the AMP partitioning measurements.
#   procps              - a ps/top that reports scheduling class and cgroup.
#
# Deliberately NOT in MED_OS_INSTALL: an empty default means a new image
# recipe ships without introspection tooling unless it asks for it, so
# med-image-prod cannot acquire it by forgetting to opt out. Development
# profiles opt in with MED_VERIFICATION_TOOLS = "${MED_VERIFICATION_TOOLSET}".
MED_VERIFICATION_TOOLSET = " \
    systemd-analyze \
    util-linux-chrt \
    util-linux-taskset \
    procps \
"

MED_VERIFICATION_TOOLS ?= ""

IMAGE_INSTALL:append = " ${MED_VERIFICATION_TOOLS}"

# The A/B disk. This belongs to MedOS, not to any device class: it is the same
# layout that makes rauc, med-data-volume and the crypttab above mean anything,
# and without it /dev/disk/by-partlabel/med-root-{a,b} does not exist and RAUC
# resolves no slots. WKS_FILE comes from MED_WKS_FILE in med-os.conf.
IMAGE_FSTYPES += "wic wic.bmap"

# How that disk boots under QEMU. Also device-class agnostic - "an EEG boots
# differently from a tomograph" would be a bug, not a feature.
#
# runqemu's default is to treat a .wic as a self-booting VM image, which for
# the EFI/GRUB layout in med-partitions.wks would mean supplying OVMF.
# "no-kernel-in-fs" makes it treat the disk as a rootfs and load the kernel
# directly instead, so QEMU gets a real partitioned disk without a bootloader
# being involved at all.
#
# QB_KERNEL_ROOT is coupled to med-partitions.wks, which puts the ESP first and
# med-root-a second. A BSP that supplies its own WKS_FILE also supplies its own
# boot path and does not use these; they are inert on a non-QEMU machine.
QB_FSINFO = "wic:no-kernel-in-fs"
QB_KERNEL_ROOT = "/dev/vda2"

# Enough headroom for the journal, a downloaded bundle and a session of
# acquisition data before /data is mounted.
IMAGE_ROOTFS_EXTRA_SPACE ?= "131072"

# A medical device boots into its application, never into a login shell.
IMAGE_LINGUAS = "en-us"

# systemd hardening knobs that belong to the image rather than to a unit file.
ROOTFS_POSTPROCESS_COMMAND += "med_harden_rootfs; "

med_harden_rootfs() {
    # RuntimeWatchdogSec: an unattended appliance must reboot itself rather
    # than sit wedged. Matches CONFIG_WATCHDOG in the kernel fragment.
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system.conf.d
    cat > ${IMAGE_ROOTFS}${sysconfdir}/systemd/system.conf.d/10-med-watchdog.conf <<EOF
[Manager]
RuntimeWatchdogSec=30s
RebootWatchdogSec=2min
EOF
}
