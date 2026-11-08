SUMMARY = "MedPlatform EEG device profile (proof of concept)"
DESCRIPTION = "The validation target of the thesis: MedOS + MedFramework + the \
AMP acquisition path + the Qt/Wayland HMI. Builds unchanged for every machine \
the platform supports - the machine is selected by the KAS project file, and \
nothing in this recipe or in the applications it installs names one."
LICENSE = "MIT"

# Cross-layer require: BBPATH spans every layer, so meta-med-app builds its
# profile on top of the MedOS base image rather than restating it. This is the
# reuse the experimental evaluation measures - the delta below is the entire
# difference between "a medical Linux platform" and "an EEG".
require recipes-core/images/med-image-base.bb

# Writable rootfs, console login, ssh and the verification tooling, all from
# one place so that this profile and med-image-tomograph cannot drift apart -
# see the note in med-image-dev.inc for why that particular drift corrupts the
# reuse metric. med-image-eeg-prod is the hardened counterpart: the same
# med-image-eeg.inc over med-image-prod.bb.
require recipes-core/images/med-image-dev.inc

require recipes-core/images/med-image-eeg.inc

# The reviewer (EDFbrowser) is a development-image program: its author limits
# its intended use, so it stays out of every production image - a regulatory
# decision, not a recipe one (implementation_plan_hmi_desktop.md, phase A3).
# Empty it together with MED_EEG_INSTALL's GUI entries for the Qt-free loop.
MED_EEG_REVIEW ?= "med-review"

IMAGE_INSTALL:append = " ${MED_EEG_REVIEW}"
