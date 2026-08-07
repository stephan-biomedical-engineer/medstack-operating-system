SUMMARY = "MedPlatform computed tomography device profile"
DESCRIPTION = "A second device class built from the same platform, and the \
control case of the experimental evaluation. A tomograph shares MedOS, \
MedFramework and the HMI graphics stack with the EEG, and differs in what it \
does not have (no real-time co-processor, so no AMP profile) and in what it \
adds (high throughput image storage). Whether that difference is small is the \
question the reuse metric answers; this recipe is where it is measured."
LICENSE = "MIT"

require recipes-core/images/med-image-base.bb

MED_ROOTFS_FEATURES = ""

IMAGE_FEATURES += " \
    ssh-server-openssh \
    debug-tweaks \
"

# Note what is reused verbatim from the EEG profile (packagegroup-med-core,
# packagegroup-med-gui) and what is absent (packagegroup-med-amp). No
# application code, no OS policy and no kernel configuration is duplicated
# between the two profiles.
MED_TOMOGRAPH_INSTALL ?= " \
    packagegroup-med-core \
    packagegroup-med-gui \
"

IMAGE_INSTALL:append = " ${MED_TOMOGRAPH_INSTALL}"

# Reconstructed slices are large; the profile is storage bound where the EEG is
# latency bound.
IMAGE_ROOTFS_EXTRA_SPACE = "524288"
