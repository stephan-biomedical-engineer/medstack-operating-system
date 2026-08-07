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

IMAGE_FEATURES += " \
    ssh-server-openssh \
    debug-tweaks \
"

# One knob for the fast inner loop: override MED_EEG_INSTALL without the GUI
# entries to build and boot the acquisition path in QEMU without compiling Qt.
#   kas build kas/project-eeg-qemu.yml \
#     --extra-conf 'MED_EEG_INSTALL = "packagegroup-med-core packagegroup-med-amp eeg-acquisition-service"'
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
