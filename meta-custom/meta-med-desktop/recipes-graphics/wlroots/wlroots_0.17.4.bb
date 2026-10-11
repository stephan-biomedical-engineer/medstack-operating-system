SUMMARY = "Modular library for building a Wayland compositor"
DESCRIPTION = "The library the station's compositor is built on: DRM/KMS \
output, libinput, seat handling through libseat, and the protocols a task \
bar needs to list and restore windows."
HOMEPAGE = "https://gitlab.freedesktop.org/wlroots/wlroots"
SECTION = "graphics"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=89e064f90bcb87796ca335cbd2ce4179"

# 0.17 and not 0.18: 0.18 needs wayland-protocols 1.35 and this series has 1.33.
SRC_URI = "git://gitlab.freedesktop.org/wlroots/wlroots.git;protocol=https;nobranch=1"
# tag 0.17.4
SRCREV = "a2d2c38a3127745629293066beeed0a649dff8de"

S = "${WORKDIR}/git"

DEPENDS = " \
    wayland wayland-native wayland-protocols \
    libdrm libxkbcommon pixman libinput udev seatd \
    hwdata libdisplay-info \
    virtual/egl virtual/libgles2 virtual/libgbm \
"

inherit meson pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "wayland opengl"

# No X11 anywhere: no nested X11 backend and no Xwayland. The distro removes
# x11 on purpose - under it any window reads the others' keys - and a
# compositor that could start an X server would quietly bring that back.
# GLES2 only; the software renderer is always built in and is what is left if
# a GPU driver will not cooperate.
EXTRA_OEMESON = " \
    -Dexamples=false \
    -Dxwayland=disabled \
    -Dxcb-errors=disabled \
    -Drenderers=gles2 \
    -Dbackends=drm,libinput \
    -Dallocators=gbm \
    -Dsession=enabled \
"

# Same redirection as libdisplay-info, for the same table: the DRM backend
# asks for a native hwdata and takes its data directory from pkg-config, which
# answers with a path on the build host.
do_configure:prepend() {
    sed -i -e "s|^\tnative: true,$||" \
           -e "s|hwdata_dir = hwdata.get_variable(pkgconfig: 'pkgdatadir')|hwdata_dir = '${STAGING_DATADIR}/hwdata'|" \
        ${S}/backend/drm/meson.build
    grep -q "hwdata_dir = '${STAGING_DATADIR}/hwdata'" ${S}/backend/drm/meson.build || \
        bbfatal "backend/drm/meson.build no longer locates hwdata the way this recipe redirects"
}
