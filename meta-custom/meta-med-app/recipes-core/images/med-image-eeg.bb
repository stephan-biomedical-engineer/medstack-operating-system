SUMMARY = "MedPlatform EEG device profile (proof of concept)"
DESCRIPTION = "The validation target of the thesis: MedOS + MedFramework + the \
AMP acquisition path + the Qt/Wayland HMI. Builds unchanged for qemux86-64 \
and for the STM32MP257 - the machine is selected by the KAS project file, and \
nothing in this recipe or in the applications it installs mentions either one."
LICENSE = "MIT"

# Cross-layer require: BBPATH spans every layer, so meta-med-app builds its
# profile on top of the MedOS base image rather than restating it. This is the
# reuse the experimental evaluation measures - the delta below is the entire
# difference between "a medical Linux platform" and "an EEG".
require recipes-core/images/med-image-base.bb

# Development profile: the rootfs stays writable and a console login is
# available, because this image exists to be inspected. med-image-prod is the
# hardened counterpart, and the OS policy underneath is identical.
MED_ROOTFS_FEATURES = ""

# This is the image the thesis runs its verification steps against, so it needs
# the tooling those steps use. med-image-test carries the same set for the
# device-class agnostic checks; see med-image-base.bb for what is in it and why
# the default is empty.
MED_VERIFICATION_TOOLS = "${MED_VERIFICATION_TOOLSET}"

IMAGE_FEATURES += " \
    ssh-server-openssh \
    debug-tweaks \
"

# One knob for the fast inner loop: override MED_EEG_INSTALL without the GUI
# entries to build and boot the acquisition path in QEMU without compiling Qt.
# Set it from a KAS fragment composed onto the project file
# (kas build a.yml:b.yml, where b.yml carries a local_conf_header) - kas build
# has no --extra-conf option. To skip the image entirely, build the recipes
# directly: make service, or bitbake eeg-acquisition-service.
MED_EEG_INSTALL ?= " \
    packagegroup-med-core \
    packagegroup-med-amp \
    packagegroup-med-gui \
    eeg-acquisition-service \
    eeg-hmi-gui \
"

IMAGE_INSTALL:append = " ${MED_EEG_INSTALL}"

# Room for a session of acquisition data on targets where /data is not a
# separate volume (QEMU).
IMAGE_ROOTFS_EXTRA_SPACE = "262144"

# The GPT disk is what makes /dev/disk/by-partlabel/med-root-{a,b} exist, and
# without those RAUC resolves no slots at all and its service dies at startup.
# It belongs to the *development* profile on purpose: med-image-prod has no
# login, and an update path that can only be inspected on an image without a
# shell is not an inspectable update path.
#
# Sizing check before changing anything here: the rootfs measures ~267 MB
# (IMAGESIZE) plus the 256 MB of IMAGE_ROOTFS_EXTRA_SPACE above, against the
# 1024 MB each slot gets in med-partitions.wks. Roughly 50% headroom, which
# disappears quickly if this profile grows.
IMAGE_FSTYPES += "wic wic.bmap"

# Boot the GPT disk without needing a bootloader inside it.
#
# runqemu's default is to treat a .wic as a self-booting VM image (see
# scripts/runqemu, "treat wic images as vmimages (with kernel) or as fsimages"),
# which for this EFI/GRUB layout would mean dragging in OVMF. "no-kernel-in-fs"
# makes it treat the disk as a rootfs instead and load the kernel directly with
# -kernel, so QEMU gets a real partitioned disk and RAUC gets real slots -
# without a bootloader, which is exactly what MED_BOOTLOADER = "noop" says we
# are testing.
#
# The rootfs is partition 2: med-partitions.wks puts the ESP first. This is
# also why there is a single QEMU boot path - QB_KERNEL_ROOT takes one value,
# and the bare ext4 image would need /dev/vda.
QB_FSINFO = "wic:no-kernel-in-fs"
QB_KERNEL_ROOT = "/dev/vda2"
