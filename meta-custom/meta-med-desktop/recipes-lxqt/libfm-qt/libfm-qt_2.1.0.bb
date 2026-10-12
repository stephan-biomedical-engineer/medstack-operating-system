SUMMARY = "The file manager's library: folder views, file operations, GIO behind Qt"
DESCRIPTION = "The library pcmanfm-qt is a thin program around. Built here \
with its X11 code removed by a patch this layer carries - see the patch for \
what goes and why nothing that remains could have used it."
HOMEPAGE = "https://github.com/lxqt/libfm-qt"
SECTION = "libs"
LICENSE = "LGPL-2.1-or-later & BSD-3-Clause"
LIC_FILES_CHKSUM = " \
    file://LICENSE;md5=435ed639f84d4585d93824e7da3d85da \
    file://LICENSE.BSD-3-Clause;md5=3775480a712fc46a69647678acb234cb \
"

SRC_URI = " \
    git://github.com/lxqt/libfm-qt.git;protocol=https;nobranch=1 \
    file://0001-build-without-X11.patch \
"
# tag 2.1.0
SRCREV = "19c49623ee99a6c44a2b35c7cd6abd56243485ac"

S = "${WORKDIR}/git"

DEPENDS = " \
    qtbase qttools qttools-native \
    glib-2.0 glib-2.0-native menu-cache libexif \
    lxqt-build-tools lxqt-menu-data \
"

inherit qt6-cmake pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"

# The patch is the reason this recipe can exist in a distribution without
# X11, so the build checks that it still does its whole job: an upstream
# release that adds one more X include would otherwise fail three minutes in,
# on a header, with nothing saying why.
do_configure:prepend() {
    if grep -rnE '<xcb/|<X11/|QX11Application|find_package\(XCB' \
            ${S}/CMakeLists.txt ${S}/src --include=CMakeLists.txt --include='*.cpp' --include='*.h' \
            | grep -v '/xdndworkaround\.' ; then
        bbfatal "libfm-qt still reaches for X11 after 0001-build-without-X11.patch; the lines are above"
    fi
}

# The CMake file this installs for its users names glib's headers and libraries
# by the absolute path they had in THIS recipe's sysroot - a directory the
# next recipe does not have, and that is deleted when this one is done. Found
# when the file manager stopped on "glib.h: No such file or directory" with
# glib in its own sysroot. The paths are rewritten to be relative to whatever
# sysroot the consumer is built against.
do_install:append() {
    targets=${D}${datadir}/cmake/fm-qt6/fm-qt6-targets.cmake
    sed -i -e "s|${RECIPE_SYSROOT}|\${CMAKE_SYSROOT}|g" "$targets"
    if grep -q "${TMPDIR}" "$targets"; then
        bbfatal "fm-qt6-targets.cmake still carries a path of this build"
    fi
}

FILES:${PN} += "${datadir}/libfm-qt6 ${datadir}/mime"
FILES:${PN}-dev += "${datadir}/cmake"

RDEPENDS:${PN} = "lxqt-menu-data shared-mime-info"
