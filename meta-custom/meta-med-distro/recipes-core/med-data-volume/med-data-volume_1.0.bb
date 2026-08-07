SUMMARY = "MedOS persistent /data volume policy (LUKS + mount unit)"
DESCRIPTION = "Owns the /data mountpoint: the crypttab entry that unlocks the \
LUKS container and the systemd mount unit that mounts it. This is the storage \
backend MedicalStorage writes patient and biomedical records to, and it is the \
only writable, update-persistent area of a MedPlatform device."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://data.mount \
    file://crypttab \
"

S = "${WORKDIR}"

inherit systemd allarch

RDEPENDS:${PN} = "cryptsetup"

SYSTEMD_SERVICE:${PN} = "data.mount"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/data.mount ${D}${systemd_system_unitdir}/data.mount

    install -d ${D}${sysconfdir}
    install -m 0600 ${WORKDIR}/crypttab ${D}${sysconfdir}/crypttab

    # The mountpoint has to exist in the image: with a read-only rootfs it
    # cannot be created at runtime.
    install -d -m 0750 ${D}/data
}

FILES:${PN} += " \
    ${systemd_system_unitdir}/data.mount \
    ${sysconfdir}/crypttab \
    /data \
"

CONFFILES:${PN} = "${sysconfdir}/crypttab"
