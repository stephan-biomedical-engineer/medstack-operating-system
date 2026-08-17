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

# The tooling that turns this repository's platform claims into measurements
# rather than assertions:
#   systemd-analyze     - "systemd-analyze security <unit>" scores the sandbox
#                         of each service, which is the evidence for the
#                         IEC 62304 5.3 software-partitioning argument.
#   util-linux-chrt     - confirms the acquisition service actually got the
#                         SCHED_RR priority its unit asks for; busybox has no
#                         chrt, and the image installs only util-linux-blkid
#                         and util-linux-lsblk, not the full util-linux.
#   util-linux-taskset  - CPU affinity, for the AMP partitioning measurements.
#   procps              - a ps/top that reports scheduling class and cgroup.
#
# Deliberately NOT in MED_OS_INSTALL: an empty default means a new image
# recipe ships without introspection tooling unless it asks for it, so
# med-image-prod cannot acquire it by forgetting to opt out. Development
# profiles opt in with MED_VERIFICATION_TOOLS = "${MED_VERIFICATION_TOOLSET}".
MED_VERIFICATION_TOOLSET = " \
    systemd-analyze \
    util-linux-chrt \
    util-linux-taskset \
    procps \
"

MED_VERIFICATION_TOOLS ?= ""

IMAGE_INSTALL:append = " ${MED_VERIFICATION_TOOLS}"

# That every MedOS device has an A/B disk belongs to MedOS: it is what makes
# rauc, med-data-volume and the crypttab above mean anything, and without it
# /dev/disk/by-partlabel/med-root-{a,b} does not exist and RAUC resolves no
# slots. *Which* disk is a board fact, and this recipe must not know it -
# WKS_FILE comes from MED_WKS_FILE, whose per-machine value lives in
# meta-med-bsp.
IMAGE_FSTYPES += "wic wic.bmap"

# How the disk boots used to be here too, as QB_FSINFO and
# QB_KERNEL_ROOT = "/dev/vda2" - a virtio device path and a partition index from
# one particular .wks, two lines under a comment asserting that this recipe is
# device-class agnostic. Both statements were true and they belonged in
# different layers; the QB_* pair is now in meta-med-bsp next to the layout it
# is coupled to.

# A machine with no disk layout produces a wic failure with no useful cause, so
# say the cause here. The check is scoped to images that actually build a wic,
# but it does fire at parse time for any build of such an image - deliberately:
# an unadapted machine is a misconfiguration, and this repository's rule is that
# a misconfiguration fails at build time rather than on a device.
python () {
    if 'wic' in (d.getVar('IMAGE_FSTYPES') or '').split() and not d.getVar('MED_WKS_FILE'):
        bb.fatal("MED_WKS_FILE is empty for MACHINE=%s: no A/B disk layout is "
                 "declared for this machine. Add a MED_WKS_FILE_DEFAULT:%s to "
                 "meta-med-bsp/conf/layer.conf pointing at a .wks in that "
                 "layer's wic/ directory (see med-partitions-efi.wks for the "
                 "policy every layout must keep), or set MED_WKS_FILE in the "
                 "KAS project file."
                 % (d.getVar('MACHINE'), d.getVar('MACHINE')))
}

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
