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

# The source tree is unpacked into a subdirectory instead of straight into
# WORKDIR. With S = ${WORKDIR}, PKGD (${WORKDIR}/package) sits *inside* S, and
# the debug-source step of do_package hardlinks every source file into
# package/usr/src/debug/... Pseudo then holds one inode under two paths, and
# re-running a task in an existing workdir aborts the build:
#   path mismatch [3 links]: ino N
#     db  '.../package/usr/src/debug/.../include/MedicalDevice.h'
#     req '.../include/MedicalDevice.h'
# Keeping S below WORKDIR also makes the cmake build tree (B, ${WORKDIR}/build)
# genuinely out-of-tree, which it was not before.
SRC_URI = " \
    file://CMakeLists.txt;subdir=sources \
    file://medframework.pc.in;subdir=sources \
    file://include;subdir=sources \
    file://src;subdir=sources \
"

S = "${WORKDIR}/sources"

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
