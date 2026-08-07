SUMMARY = "MedOS production image (read-only rootfs, A/B, no shell access)"
DESCRIPTION = "The shippable form of the MedOS base: immutable rootfs, no \
debug features, no remote login, and a WIC image carrying the full A/B GPT \
layout so it can be written to a device with bmaptool. Device profiles derive \
their production variants from this recipe."
LICENSE = "MIT"

require recipes-core/images/med-image-base.bb

# Inherited from med-image-base, restated because it is the defining property
# of this image and must not be lost to an override further down.
MED_ROOTFS_FEATURES = "read-only-rootfs"

# Strip anything that would weaken the deployed device, including features a
# developer's local.conf may have added through EXTRA_IMAGE_FEATURES.
IMAGE_FEATURES:remove = "debug-tweaks tools-debug tools-profile dbg-pkgs \
                         empty-root-password allow-empty-password \
                         allow-root-login post-install-logging"

# The full disk image: ESP + med-root-a + med-root-b + med-data.
# WKS_FILE comes from med-os.conf ("?=") so a BSP can override it.
IMAGE_FSTYPES += "wic wic.bmap"

# Nothing gets installed here that is not already in the base: a production
# image differs from the base by what it *removes*, which keeps the two in
# lockstep and makes the diff between them auditable.
