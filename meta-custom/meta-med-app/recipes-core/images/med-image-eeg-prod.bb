SUMMARY = "MedPlatform EEG device profile, production image"
DESCRIPTION = "The EEG profile over the hardened MedOS image: read-only \
rootfs, no debug features, no login and no remote shell. The same \
med-image-eeg.inc the development image installs, so the two differ by \
their base and by the reviewer, never by the device class."
LICENSE = "MIT"

# The production policy (med-image-prod.bb requires med-image-base.bb and
# removes debug-tweaks and the rest), never med-image-dev.inc: production
# differs from development by not opting in.
require recipes-core/images/med-image-prod.bb

require recipes-core/images/med-image-eeg.inc

# Serviced through a signed grant (med-image-prod.bb, MED_SERVICE_ACCESS).
MED_SERVICE_ACCESS = "med-service-access"
