SUMMARY = "MedOS service access: authenticated, off by default, audited"
DESCRIPTION = "How a production MedOS device is serviced without a shell \
anyone can reach: a grant signed with the update keyring, on a removable \
medium, opens an SSH window of bounded duration for one service user and one \
key, and every step - granted, refused, each connection, closed - is an audit \
record in the MedicalLogger chain. ACE-03 of docs/CONFORMIDADE.md; IEC \
81001-5-1 5.3.2 g)-h)."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://CMakeLists.txt;subdir=sources \
    file://src;subdir=sources \
    file://start \
    file://stop \
    file://med-trail-export \
    file://med-service-access.service \
    file://med-service-sshd.socket \
    file://med-service-sshd@.service \
    file://med-service.sshd_config \
    file://90-med-service-access.rules \
"
S = "${WORKDIR}/sources"

# The framework (MedicalLogger: the audit chain) and libcrypto (the grant's
# CMS signature) - the framework's own two dependencies, not new ones.
DEPENDS = "med-framework-api openssl"

inherit cmake pkgconfig useradd

# The one account service access logs in as. Its password field is "*": no
# password works, and unlike "!" sshd does not read it as a locked account,
# so the key is the only way in. In systemd-journal to read the trail.
USERADD_PACKAGES = "${PN}"
USERADD_PARAM:${PN} = "--system --home-dir /run/med-service --no-create-home \
                       --shell /bin/sh --password '*' --groups systemd-journal med-service"
USERADD_DEPENDS = "systemd"
# systemd's preinst creates systemd-journal; without a runtime dependency opkg
# may run this package's preinst first, and useradd fails on the missing group
# (seen on the first production build). Both: the order, and the group itself
# (useradd.bbclass skips a group that already exists).
GROUPADD_PARAM:${PN} = "-r systemd-journal"

do_install:append() {
    install -d ${D}${libexecdir}/med-service-access
    install -m 0755 ${WORKDIR}/start ${WORKDIR}/stop ${D}${libexecdir}/med-service-access/
    install -m 0755 ${WORKDIR}/med-trail-export ${D}${bindir}/
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/med-service-access.service ${WORKDIR}/med-service-sshd.socket \
        ${WORKDIR}/med-service-sshd@.service ${D}${systemd_system_unitdir}/
    install -d ${D}${sysconfdir}/ssh
    install -m 0644 ${WORKDIR}/med-service.sshd_config ${D}${sysconfdir}/ssh/
    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/90-med-service-access.rules ${D}${nonarch_base_libdir}/udev/rules.d/
}

FILES:${PN} += " \
    ${libexecdir}/med-service-access \
    ${systemd_system_unitdir} \
    ${nonarch_base_libdir}/udev/rules.d \
"

# The daemon and the host-key generator only; the stock sshd.socket the
# openssh-sshd package enables is switched off by the production image
# (med-image-prod.bb), which is the image this package exists for.
RDEPENDS:${PN} = "systemd openssh-sshd openssh-keygen util-linux-blkid med-framework-api-audit-verify"
