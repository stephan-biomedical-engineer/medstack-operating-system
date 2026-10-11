SUMMARY = "EDID and DisplayID library"
DESCRIPTION = "Parses the description a monitor gives of itself. A build \
dependency of the compositor library's DRM backend."
HOMEPAGE = "https://gitlab.freedesktop.org/emersion/libdisplay-info"
SECTION = "graphics"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=e4426409957080ee0352128354cea2de"

SRC_URI = "git://gitlab.freedesktop.org/emersion/libdisplay-info.git;protocol=https;nobranch=1"
# tag 0.1.1
SRCREV = "92b031749c0fe84ef5cdf895067b84a829920e25"

S = "${WORKDIR}/git"

DEPENDS = "hwdata"

inherit meson pkgconfig python3native

# The build reads the PNP vendor table from hwdata. It asks pkg-config for a
# NATIVE hwdata, which this layer set does not build, and falls back to
# /usr/share/hwdata on the build host - a file the build container does not
# have and a build must not read anyway. The table is data, the same for every
# architecture, so the target's copy in the recipe sysroot is the right one.
do_configure:prepend() {
    sed -i -e "s|files('/usr/share/hwdata/pnp.ids')|files('${STAGING_DATADIR}/hwdata/pnp.ids')|" \
        ${S}/meson.build
    grep -q "${STAGING_DATADIR}/hwdata/pnp.ids" ${S}/meson.build || \
        bbfatal "meson.build no longer names the hwdata fallback this recipe redirects"
}
