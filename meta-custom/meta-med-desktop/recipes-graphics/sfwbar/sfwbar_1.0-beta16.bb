SUMMARY = "Task bar for Wayland compositors"
DESCRIPTION = "The station's task bar: lists the open windows and brings a \
minimised one back when it is clicked, through the foreign-toplevel protocol \
the compositor offers. That one function is why it is here; everything else \
it can show - network, volume, battery, a clock - is built out or left out \
of the configuration."
HOMEPAGE = "https://github.com/LBCrion/sfwbar"
SECTION = "graphics"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1ebbd3e34237af26da5dc08a4e440464"

SRC_URI = " \
    git://github.com/LBCrion/sfwbar.git;protocol=https;nobranch=1 \
    file://0001-trigger-return-from-the-main-context-callback.patch \
    file://sfwbar.config \
"
# tag v1.0_beta16
SRCREV = "acd84a0713329a81647d363f36a11214a1923bdc"

S = "${WORKDIR}/git"

DEPENDS = "gtk+3 gtk-layer-shell json-c glib-2.0 wayland wayland-native wayland-protocols"

inherit meson pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"

# Every module that talks to something this device does not have or must not
# reach is off: sound, bluetooth, wireless, a network indicator on a station
# whose desktop has no network namespace to look at.
EXTRA_OEMESON = " \
    -Dalsa=disabled \
    -Dpulse=disabled \
    -Dmpd=disabled \
    -Dbluez=disabled \
    -Diwd=disabled \
    -Dnm=disabled \
    -Dnetwork=disabled \
    -Dncenter=disabled \
    -Dxkb=disabled \
    -Dbsdctl=disabled \
    -Dbuild-docs=disabled \
"

do_install:append() {
    install -D -m 0644 ${WORKDIR}/sfwbar.config ${D}${sysconfdir}/xdg/sfwbar/sfwbar.config
}

FILES:${PN} += "${libdir}/sfwbar ${datadir}/sfwbar ${datadir}/icons ${sysconfdir}/xdg/sfwbar"

RDEPENDS:${PN} = "adwaita-icon-theme"
