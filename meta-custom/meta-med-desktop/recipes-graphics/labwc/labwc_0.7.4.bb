SUMMARY = "A stacking Wayland compositor in the manner of Openbox"
DESCRIPTION = "The station's compositor: windows with a frame the compositor \
draws, that can be minimised, and that a task bar can list and bring back - \
the thing the operator asked for and the previous compositor could not grow."
HOMEPAGE = "https://labwc.github.io"
SECTION = "graphics"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

# 0.7.x is the series built on wlroots 0.17.
SRC_URI = "git://github.com/labwc/labwc.git;protocol=https;nobranch=1"
# tag 0.7.4
SRCREV = "71136fdf65940b4e924dea7f8285d2a21032755e"

S = "${WORKDIR}/git"

DEPENDS = " \
    wlroots wayland wayland-native wayland-protocols \
    libxkbcommon libdrm libxml2 glib-2.0 cairo pango libinput pixman libpng \
"

inherit meson pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"

# No Xwayland, for the reason given in the wlroots recipe. No SVG buttons: they
# would bring librsvg into an image that has no other use for it, and the
# built-in buttons are drawn without it.
EXTRA_OEMESON = " \
    -Dxwayland=disabled \
    -Dsvg=disabled \
    -Dman-pages=disabled \
    -Dnls=disabled \
"

FILES:${PN} += "${datadir}/wayland-sessions ${datadir}/xdg-desktop-portal ${datadir}/icons"
