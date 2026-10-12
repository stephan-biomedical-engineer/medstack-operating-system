SUMMARY = "Output configuration tool for wlroots compositors"
DESCRIPTION = "Sets an output's mode through the compositor's output \
management protocol. The station's compositor has no configuration for \
output modes of its own; this is how the session pins the one mode a board's \
display chain needs, and how a bench session reads which mode is programmed."
HOMEPAGE = "https://sr.ht/~emersion/wlr-randr/"
SECTION = "graphics"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=07e8a8f2dc9e6a7f131e81037398c61b"

SRC_URI = "git://git.sr.ht/~emersion/wlr-randr;protocol=https;nobranch=1"
# tag v0.4.1
SRCREV = "11f876d0a6543a3923be9967931b89395b20116b"

S = "${WORKDIR}/git"

DEPENDS = "wayland wayland-native"

inherit meson pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland"
