SUMMARY = "MedPlatform EEG profile as a container for a desktop Linux host"
DESCRIPTION = "med-image-eeg, minus what a host already is: no disk layout, \
no A/B update, no encrypted volume and no front-end kernel modules. Shipped \
as a root filesystem archive that systemd-nspawn boots on the host \
(host/README.md). The framework, the service, the sealed configuration, the \
HMI and the units are the same packages the device profiles install."
LICENSE = "MIT"

# The EEG profile itself, not a restatement of it: whatever med-image-eeg gains
# later, this image gains too.
require recipes-core/images/med-image-eeg.bb

# --- A directory tree, not a disk --------------------------------------------
# "=" and not ":remove": the base image adds wic with "+=", the machine conf
# adds its own types, and none of them means anything to a container. The base
# image's MED_WKS_FILE check is scoped to images that build a wic, so it stays
# silent here by construction.
IMAGE_FSTYPES = "tar.zst"

# --- What the host is ---------------------------------------------------------
# The encrypted record volume is a partition and its provisioning; on the host
# /data is a directory bound in by the container's unit, and whether that
# directory is encrypted is the host's business (MED_EEG_REQUIRE_ENCRYPTION is
# "false" in kas/project-eeg-host.yml, and the session metadata says so).
MED_HOST_NOT_HERE = " \
    med-data-volume \
    cryptsetup \
    e2fsprogs-mke2fs \
    e2fsprogs-resize2fs \
    ${MED_AFE_INSTALL} \
"
# MED_AFE_INSTALL is the front-end's kernel half: its modules, the udev rule
# that names it and the module options. On a host those are installed on the
# host (host/install.sh), because a container has neither a kernel of its own
# nor a udev. The service still talks to the converter the same way - through
# /dev/med-afe-eeg0 and /run/med-afe/eeg0, which the container is given.
IMAGE_INSTALL:remove = "${MED_HOST_NOT_HERE}"

# --- RAUC stays, inert -------------------------------------------------------
# rauc is a runtime dependency of the framework (MedicalUpdate is its D-Bus
# client), so it is installed. With no slots there is nothing for it to do,
# and the unit that confirms a slot would fail every boot; the host updates
# this tree by replacing it. Masked rather than removed, so that the framework
# package is byte-identical to the device's.
ROOTFS_POSTPROCESS_COMMAND += "med_host_mask_units; "

med_host_mask_units() {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system
    for unit in rauc.service rauc-mark-good.service; do
        ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/$unit
    done
}

# The size the device profile reserves for a session on a rootfs without a
# /data volume does not apply: /data is the host's directory.
IMAGE_ROOTFS_EXTRA_SPACE = "0"
