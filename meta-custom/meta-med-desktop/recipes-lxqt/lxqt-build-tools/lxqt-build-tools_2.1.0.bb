SUMMARY = "CMake modules the LXQt components are built with"
DESCRIPTION = "Build-time only: the CMake modules and helper scripts that \
libfm-qt and pcmanfm-qt find as lxqt2-build-tools. Nothing of it is installed \
on a device."
HOMEPAGE = "https://github.com/lxqt/lxqt-build-tools"
SECTION = "devel"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://BSD-3-Clause;md5=a8987c1fd6930fe14ae46b4d72e53770"

SRC_URI = "git://github.com/lxqt/lxqt-build-tools.git;protocol=https;nobranch=1"
# tag 2.1.0
SRCREV = "6754f1307fc146023f2d8c3129830b7ec34dba43"

S = "${WORKDIR}/git"

DEPENDS = "qtbase"

inherit qt6-cmake

# CMake modules and two scripts, the same for every architecture.
FILES:${PN}-dev += "${datadir}/cmake ${bindir}"
ALLOW_EMPTY:${PN} = "1"
