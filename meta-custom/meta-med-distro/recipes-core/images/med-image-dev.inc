# MedOS development profile.
#
# Everything that makes an image inspectable rather than shippable, in one
# place, so that device profiles differ from each other only by what they
# install.
#
# WHY THIS FILE EXISTS
# med-image-eeg and med-image-tomograph each carried their own copy of these
# settings, and they drifted: the QEMU boot configuration and the verification
# toolset landed in the EEG profile alone, so the tomograph could not boot the
# same way and could not be measured the same way. That matters more here than
# it would elsewhere, because med-image-tomograph describes itself as "the
# control case of the experimental evaluation" - and the reuse metric is only
# meaningful if the two profiles differ *exclusively* by device class. Drift
# between them is measured as if it were device-class difference.
#
# A device profile requires this file after med-image-base.bb. Production
# profiles require only the base, and therefore cannot acquire any of this by
# forgetting to opt out.

# The one deliberate deviation from production policy. Empty rather than
# "read-only-rootfs"; med-image-base applies it through IMAGE_FEATURES, which
# is expanded lazily, so the order of the two requires does not matter.
MED_ROOTFS_FEATURES = ""

IMAGE_FEATURES += " \
    ssh-server-openssh \
    debug-tweaks \
"

# Platform-policy introspection: sandbox exposure scores, scheduling class,
# CPU affinity. See MED_VERIFICATION_TOOLSET in med-image-base.bb for what is
# in it and why its default is empty.
MED_VERIFICATION_TOOLS = "${MED_VERIFICATION_TOOLSET}"
