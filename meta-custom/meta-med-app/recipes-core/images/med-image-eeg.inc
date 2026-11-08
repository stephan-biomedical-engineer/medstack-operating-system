# The EEG device class, once, for both of its images.
#
# med-image-eeg (development) and med-image-eeg-prod (production) require this
# file over different bases - med-image-dev.inc on one side, med-image-prod.bb
# on the other - so the two differ by the base and by nothing the profile
# installs. Phase 5.3 of docs/implementation_plan_conformity.md; the package
# difference between the two images is a number in docs/RESULTS.md.

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
# layout (meta-med-bsp/wic/), i.e. about 60% used. The wic and QEMU boot
# settings themselves are in med-image-base.bb - they are platform properties,
# not EEG ones.
