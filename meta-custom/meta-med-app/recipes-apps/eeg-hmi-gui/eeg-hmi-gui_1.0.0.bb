SUMMARY = "MedPlatform EEG operator display (Qt/Wayland HMI)"
DESCRIPTION = "Qt Quick front-end of the EEG proof of concept. Subscribes to \
the sample stream published by eeg-acquisition-service over MedicalIPC and \
renders it. It holds no patient records, opens no device and reaches no \
network - the partition between the safety relevant acquisition path and the \
non safety relevant display is enforced by the sandbox in its unit file, not \
only asserted in a document."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://CMakeLists.txt \
    file://src \
    file://qml \
    file://eeg-hmi.service \
"

S = "${WORKDIR}"

DEPENDS = "qtbase qtdeclarative med-framework-api"

inherit qt6-cmake pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd wayland opengl"

RDEPENDS:${PN} = " \
    qtwayland \
    qtdeclarative-qmlplugins \
"

SYSTEMD_SERVICE:${PN} = "eeg-hmi.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/eeg-hmi.service \
        ${D}${systemd_system_unitdir}/eeg-hmi.service
}

FILES:${PN} += "${systemd_system_unitdir}/eeg-hmi.service"
