SUMMARY = "Extra CMake modules, from the KDE project"
DESCRIPTION = "Build-time only: the CMake modules layer-shell-qt is built \
with. Nothing of it is installed on a device."
HOMEPAGE = "https://invent.kde.org/frameworks/extra-cmake-modules"
SECTION = "devel"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSES/BSD-3-Clause.txt;md5=954f4d71a37096249f837652a7f586c0"

SRC_URI = "git://invent.kde.org/frameworks/extra-cmake-modules.git;protocol=https;nobranch=1"
# tag v6.8.0
SRCREV = "02e39e9538d3699e470194c989f7047efe44d2cc"

S = "${WORKDIR}/git"

inherit cmake

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DBUILD_HTML_DOCS=OFF \
    -DBUILD_MAN_DOCS=OFF \
    -DBUILD_QTHELP_DOCS=OFF \
"

FILES:${PN}-dev += "${datadir}/ECM"
# One of the modules ships a git hook written in Python. It is a tool for
# whoever develops against these modules, in a -dev package no image installs;
# giving it a Python at run time would be a dependency for nothing.
INSANE_SKIP:${PN}-dev += "file-rdeps"
ALLOW_EMPTY:${PN} = "1"
