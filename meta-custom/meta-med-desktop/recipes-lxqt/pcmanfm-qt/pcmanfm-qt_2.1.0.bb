SUMMARY = "File manager (Qt), for the station desktop"
DESCRIPTION = "The file manager the operator asked the station to have. It \
runs on Wayland as shipped but is compiled against X11 headers; this layer \
carries a patch that removes the one place it uses them. It is third-party \
software run as the desktop's own user, and what it can open is what that \
user can: the clinical files of closed sessions, and nothing of the raw \
record."
HOMEPAGE = "https://github.com/lxqt/pcmanfm-qt"
SECTION = "x11"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://LICENSE;md5=4641e94ec96f98fabc56ff9cc48be14b"

SRC_URI = " \
    git://github.com/lxqt/pcmanfm-qt.git;protocol=https;nobranch=1 \
    file://0001-build-without-X11.patch \
"
# tag 2.1.0
SRCREV = "07006b796baac4c88d2f82d0fc60f592ffa56ad9"

S = "${WORKDIR}/git"

DEPENDS = " \
    qtbase qttools qttools-native \
    libfm-qt lxqt-build-tools layer-shell-qt \
"

inherit qt6-cmake pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"

# Same check as libfm-qt's, for the same reason.
do_configure:prepend() {
    if grep -rnE '^#include <xcb/|^#include <X11/|nativeInterface<QNativeInterface::QX11Application>' \
            ${S}/pcmanfm --include='*.cpp' --include='*.h' ; then
        bbfatal "pcmanfm-qt still reaches for X11 after 0001-build-without-X11.patch; the lines are above"
    fi
}

FILES:${PN} += "${datadir}/pcmanfm-qt ${datadir}/icons ${sysconfdir}/xdg"

RDEPENDS:${PN} = "qtwayland adwaita-icon-theme"
