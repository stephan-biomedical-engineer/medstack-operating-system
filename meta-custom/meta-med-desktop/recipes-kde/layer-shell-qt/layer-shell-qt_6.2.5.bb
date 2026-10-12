SUMMARY = "Qt component for the Wayland layer-shell protocol"
DESCRIPTION = "Lets a Qt program anchor a surface to a screen edge or put it \
behind every window. The file manager links it for the one feature this \
station does not use, drawing the desktop, and will not build without it."
HOMEPAGE = "https://invent.kde.org/plasma/layer-shell-qt"
SECTION = "graphics"
LICENSE = "LGPL-3.0-or-later"
LIC_FILES_CHKSUM = "file://LICENSES/LGPL-3.0-or-later.txt;md5=3000208d539ec061b899bce1d9ce9404"

SRC_URI = "git://invent.kde.org/plasma/layer-shell-qt.git;protocol=https;nobranch=1"
# tag v6.2.5
SRCREV = "c04ded0400d61f4e86afbb894d8d55e6845abb28"

S = "${WORKDIR}/git"

DEPENDS = " \
    qtbase qtdeclarative qtdeclarative-native qtwayland qtwayland-native \
    wayland wayland-native wayland-protocols libxkbcommon \
    extra-cmake-modules \
"

inherit qt6-cmake pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"

EXTRA_OECMAKE = "-DBUILD_TESTING=OFF"

FILES:${PN} += "${libdir}/plugins ${libdir}/qml"
FILES:${PN}-dev += "${libdir}/cmake"
