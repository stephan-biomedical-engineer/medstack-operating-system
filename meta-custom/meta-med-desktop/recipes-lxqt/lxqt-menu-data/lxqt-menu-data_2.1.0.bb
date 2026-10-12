SUMMARY = "Menu definition files for LXQt"
DESCRIPTION = "The freedesktop.org menu files the file manager's library \
reads to build its 'Applications' tree and its 'open with' lists."
HOMEPAGE = "https://github.com/lxqt/lxqt-menu-data"
SECTION = "x11"
LICENSE = "LGPL-2.1-or-later"
LIC_FILES_CHKSUM = "file://LICENSE;md5=0964c689fcf4c21c6797ea87408416b6"

SRC_URI = "git://github.com/lxqt/lxqt-menu-data.git;protocol=https;nobranch=1"
# tag 2.1.0
SRCREV = "dd9ed8e309d8a605ede1e69c0b6e7ba1423bc731"

S = "${WORKDIR}/git"

DEPENDS = "qtbase qttools qttools-native lxqt-build-tools"

inherit qt6-cmake

FILES:${PN} += "${datadir}/desktop-directories ${sysconfdir}/xdg/menus ${datadir}/lxqt"
FILES:${PN}-dev += "${datadir}/cmake"
