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
