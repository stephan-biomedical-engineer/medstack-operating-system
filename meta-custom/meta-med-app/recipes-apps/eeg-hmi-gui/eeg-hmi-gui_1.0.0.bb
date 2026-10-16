SUMMARY = "MedPlatform EEG operator display (Qt/Wayland HMI)"
DESCRIPTION = "Qt Quick front-end of the EEG proof of concept. Subscribes to \
the sample stream published by eeg-acquisition-service over MedicalIPC and \
renders it. It holds no patient records, opens no device and reaches no \
network - the partition between the safety relevant acquisition path and the \
non safety relevant display is enforced by the sandbox in its unit file, not \
only asserted in a document."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Source tree in a subdirectory, not directly in WORKDIR - see the same note in
# med-framework-api_1.0.0.bb: S = ${WORKDIR} puts PKGD inside S and the
# debug-source hardlinks of do_package make pseudo abort on any task re-run.
# The unit file stays in WORKDIR because do_install reads it from there.
SRC_URI = " \
    file://CMakeLists.txt;subdir=sources \
    file://src;subdir=sources \
    file://qml;subdir=sources \
    file://eeg-hmi.service \
    file://eeg-hmi-request.path \
    file://eeg-hmi-request.service \
    file://eeg-hmi-request.conf \
    file://desktop-request.conf \
    file://signals.desktop \
"

S = "${WORKDIR}/sources"

# qtdeclarative-native is not optional for a QML application: qt6-cmake.bbclass
# only pulls qtbase-native and points QT_HOST_PATH at it, so the target
# Qt6QmlConfig.cmake finds no Qt6QmlTools/Qt6QuickTools (qmlcachegen,
# qmltyperegistrar, qmlimportscanner) and reports Qt6Qml as NOT FOUND.
DEPENDS = "qtbase qtdeclarative qtdeclarative-native med-framework-api"

inherit qt6-cmake pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd wayland opengl"

RDEPENDS:${PN} = " \
    qtwayland \
    qtdeclarative-qmlplugins \
"

# The display at boot, and on request afterwards: the path is what brings it
# back when nothing else would (eeg-hmi-request.path says when that was).
SYSTEMD_SERVICE:${PN} = "eeg-hmi.service eeg-hmi-request.path"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/eeg-hmi.service \
        ${D}${systemd_system_unitdir}/eeg-hmi.service
    install -m 0644 ${WORKDIR}/eeg-hmi-request.path \
        ${D}${systemd_system_unitdir}/eeg-hmi-request.path
    install -m 0644 ${WORKDIR}/eeg-hmi-request.service \
        ${D}${systemd_system_unitdir}/eeg-hmi-request.service
    install -D -m 0644 ${WORKDIR}/eeg-hmi-request.conf \
        ${D}${nonarch_libdir}/tmpfiles.d/eeg-hmi-request.conf
    # Where the station desktop takes its icons from, and leave for it to ask.
    # Both inert in an image without that desktop. Neither names the desktop's
    # program: the icon goes where any desktop entry of the station goes, and
    # the drop-in only says which path its unit may write.
    install -D -m 0644 ${WORKDIR}/signals.desktop \
        ${D}${datadir}/medplatform/desktop/signals.desktop
    install -D -m 0644 ${WORKDIR}/desktop-request.conf \
        ${D}${systemd_system_unitdir}/med-desktop.service.d/eeg-hmi-request.conf
}

FILES:${PN} += " \
    ${systemd_system_unitdir}/eeg-hmi.service \
    ${systemd_system_unitdir}/eeg-hmi-request.path \
    ${systemd_system_unitdir}/eeg-hmi-request.service \
    ${systemd_system_unitdir}/med-desktop.service.d/eeg-hmi-request.conf \
    ${nonarch_libdir}/tmpfiles.d/eeg-hmi-request.conf \
    ${datadir}/medplatform/desktop/signals.desktop \
"
