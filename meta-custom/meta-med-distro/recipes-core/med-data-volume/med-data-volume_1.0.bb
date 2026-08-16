SUMMARY = "MedOS persistent /data volume policy (LUKS + provisioning + mount unit)"
DESCRIPTION = "Owns the /data mountpoint: the first-boot step that converts the \
med-data partition into a LUKS volume, and the systemd mount unit that mounts \
it. This is the storage backend MedicalStorage writes patient and biomedical \
records to, and it is the only writable, update-persistent area of a \
MedPlatform device."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://data.mount \
    file://crypttab \
    file://med-data-provision.sh \
    file://med-data-provision.service \
"

S = "${WORKDIR}"

inherit systemd allarch

# blkid reads the pristine filesystem label the provisioning safeguard keys on;
# mke2fs creates the filesystem inside the container. Both are already in
# MED_OS_INSTALL, but the package that needs them should say so.
RDEPENDS:${PN} = "cryptsetup util-linux-blkid e2fsprogs-mke2fs"

SYSTEMD_SERVICE:${PN} = "med-data-provision.service data.mount"
SYSTEMD_AUTO_ENABLE = "enable"

# Where the volume key comes from. Defaults to tpm2 rather than development,
# and the direction is the point: a target that forgets to declare a key source
# fails to provision and refuses to run, instead of silently shipping a device
# that looks encrypted and is not. Set per target in the KAS project file, the
# same way MED_EEG_DRIVER, MED_BOOTLOADER and MED_WKS_FILE are.
MED_DATA_KEY_SOURCE ?= "tpm2"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/data.mount ${D}${systemd_system_unitdir}/data.mount
    install -m 0644 ${WORKDIR}/med-data-provision.service \
        ${D}${systemd_system_unitdir}/med-data-provision.service

    install -d ${D}${libexecdir}/medplatform
    sed -e "s|@MED_DATA_KEY_SOURCE@|${MED_DATA_KEY_SOURCE}|g" \
        ${WORKDIR}/med-data-provision.sh \
        > ${D}${libexecdir}/medplatform/med-data-provision.sh
    chmod 0750 ${D}${libexecdir}/medplatform/med-data-provision.sh

    # A leftover placeholder would ship a device whose provisioning step decides
    # its key source from a literal string that matches nothing, and the failure
    # would only surface on a device. Fail the build instead.
    if grep -q "@MED_[A-Z_]*@" ${D}${libexecdir}/medplatform/med-data-provision.sh; then
        bbfatal "unsubstituted placeholder left in med-data-provision.sh"
    fi

    install -d ${D}${sysconfdir}
    install -m 0600 ${WORKDIR}/crypttab ${D}${sysconfdir}/crypttab

    # The mountpoint has to exist in the image: with a read-only rootfs it
    # cannot be created at runtime.
    install -d -m 0750 ${D}/data
}

FILES:${PN} += " \
    ${systemd_system_unitdir}/data.mount \
    ${systemd_system_unitdir}/med-data-provision.service \
    ${libexecdir}/medplatform/med-data-provision.sh \
    ${sysconfdir}/crypttab \
    /data \
"

CONFFILES:${PN} = "${sysconfdir}/crypttab"
