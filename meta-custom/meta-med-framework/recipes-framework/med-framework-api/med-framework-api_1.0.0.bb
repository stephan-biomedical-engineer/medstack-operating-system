SUMMARY = "MedFramework - medical abstraction API (MedPlatform layer 3)"
DESCRIPTION = "Six C++ abstractions every MedPlatform application is built on: \
MedicalIPC (OpenAMP rpmsg and local IPC), MedicalLogger (tamper-evident audit \
trail on journald), MedicalStorage (encrypted record persistence), \
MedicalUpdate (RAUC A/B over D-Bus), MedicalConfiguration (calibration and \
safety limits) and MedicalDevice (sensor/actuator drivers). Applications link \
against this and never against the OS or the BSP directly - that isolation is \
what makes a device profile portable across hardware."
HOMEPAGE = "https://github.com/stephan-biomedical-engineer"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://CMakeLists.txt \
    file://medframework.pc.in \
    file://include \
    file://src \
"

S = "${WORKDIR}"

DEPENDS = "systemd openssl"

inherit cmake pkgconfig features_check

# sd-journal and sd-bus are not optional implementation details here - they are
# the mechanisms MedicalLogger and MedicalUpdate are specified in terms of.
# Fail at parse time on a distro without systemd instead of at link time.
REQUIRED_DISTRO_FEATURES = "systemd"

FILES:${PN} += "${libdir}/libmedframework.so.*"
FILES:${PN}-dev += " \
    ${includedir}/medplatform \
    ${libdir}/pkgconfig/medframework.pc \
"

# The shared library is what applications RDEPEND on; nothing else in the image
# needs it.
RDEPENDS:${PN} = ""
