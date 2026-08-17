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

# Writable rootfs, console login, ssh and the verification tooling, all from
# one place so that this profile and med-image-tomograph cannot drift apart -
# see the note in med-image-dev.inc for why that particular drift corrupts the
# reuse metric. med-image-prod is the hardened counterpart and requires only
# the base; the OS policy underneath is identical.
require recipes-core/images/med-image-dev.inc

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

# Sizing note kept with the profile that has to live inside it: this rootfs
# measures ~609 MiB against the 1024 MiB each slot gets in every med-partitions
# layout (meta-med-bsp/wic/),
# i.e. about 60% used. The wic and QEMU boot settings themselves are in
# med-image-base.bb - they are platform properties, not EEG ones.
