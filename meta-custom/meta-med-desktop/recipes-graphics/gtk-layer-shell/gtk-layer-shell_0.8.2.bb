SUMMARY = "Library to create panels and other desktop components for Wayland with GTK 3"
DESCRIPTION = "Lets a GTK 3 program anchor a surface to a screen edge and \
reserve space there - what makes a panel a panel rather than a window. A \
build dependency of the station's task bar."
HOMEPAGE = "https://github.com/wmww/gtk-layer-shell"
SECTION = "graphics"
# The library is MIT, with files carried over from GTK under the LGPL. The
# GPL text in the tree covers the tests and examples, which are not built.
LICENSE = "MIT & LGPL-3.0-or-later"
LIC_FILES_CHKSUM = " \
    file://LICENSE_MIT.txt;md5=ffeb3391e5dace600b84c757688b9f1b \
    file://LICENSE_LGPL.txt;md5=3000208d539ec061b899bce1d9ce9404 \
"

SRC_URI = "git://github.com/wmww/gtk-layer-shell.git;protocol=https;nobranch=1"
# tag v0.8.2
SRCREV = "91e5ef02b557f93337bcc11ffe8c0a251aa9ab52"

S = "${WORKDIR}/git"

DEPENDS = "gtk+3 wayland wayland-native wayland-protocols"

inherit meson pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"

# Only the C library: no bindings for languages this image has no interpreter
# for.
EXTRA_OEMESON = " \
    -Dintrospection=false \
    -Dvapi=false \
    -Dexamples=false \
    -Ddocs=false \
    -Dtests=false \
"
