SUMMARY = "MedOS journal sealing: provisions the Forward Secure Sealing key"
DESCRIPTION = "Seal=yes in the journald policy seals nothing until a sealing \
key exists. This recipe ships the first-boot step that creates one per \
machine-id and hands the verification key over once, through a tmpfs file, so \
that it can leave the device - see med-journal-seal.sh for why it must."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://med-journal-seal.sh \
    file://med-journal-seal.service \
"

S = "${WORKDIR}"

# No machine, bootloader or key-store variable reaches this recipe: the journal
# directory and the machine-id are the same on every MedOS target.
inherit allarch systemd

# journalctl does the work; it is in the systemd package, built with gcrypt
# because med-os.conf asks for it (FSS needs it).
RDEPENDS:${PN} = "systemd"

SYSTEMD_SERVICE:${PN} = "med-journal-seal.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${libexecdir}/medplatform
    install -m 0755 ${S}/med-journal-seal.sh ${D}${libexecdir}/medplatform/med-journal-seal.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/med-journal-seal.service ${D}${systemd_system_unitdir}/med-journal-seal.service
}

FILES:${PN} = " \
    ${libexecdir}/medplatform/med-journal-seal.sh \
    ${systemd_system_unitdir}/med-journal-seal.service \
"
