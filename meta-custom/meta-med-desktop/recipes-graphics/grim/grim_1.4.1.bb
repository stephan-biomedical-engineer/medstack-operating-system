SUMMARY = "Screenshot tool for Wayland compositors"
DESCRIPTION = "Writes what the compositor is showing to a PNG. A tool of the \
desktop trial and of its acceptance: it is how a headless QEMU run, or a \
bench session nobody is watching, can say what was on the screen."
HOMEPAGE = "https://gitlab.freedesktop.org/emersion/grim"
SECTION = "graphics"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=e316e9609dd7672b87ff25b46b2cf3e1"

SRC_URI = "git://gitlab.freedesktop.org/emersion/grim.git;protocol=https;nobranch=1"
# tag v1.4.1
SRCREV = "7ba46364ab95141c79e0e18093aa66597256182c"

S = "${WORKDIR}/git"

DEPENDS = "wayland wayland-native wayland-protocols pixman libpng"

inherit meson pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"

EXTRA_OEMESON = "-Djpeg=disabled -Dman-pages=disabled"
