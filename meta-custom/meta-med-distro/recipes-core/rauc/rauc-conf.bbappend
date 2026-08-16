# MedOS A/B update policy.
#
# meta-rauc ships an (empty) rauc-conf recipe whose only job is to package
# /etc/rauc/system.conf; the integrator is expected to override the file
# through the file search path. Prepending THISDIR/${PN} makes our system.conf
# win without touching meta-rauc's SRC_URI.
#
# NOTE: the filename must be "rauc-conf.bbappend", NOT "rauc-conf_%.bbappend".
# meta-rauc/scarthgap ships the recipe unversioned as rauc-conf.bb, and bitbake
# matches appends against the recipe *filename*: the "_%" form requires an
# underscore in the .bb name, so it matches nothing here and bitbake fails hard
# with "No recipes in default available for: .../rauc-conf_%.bbappend".
# Re-check after any meta-rauc bump with:
#   kas shell kas/kas-base.yml -c "bitbake-layers show-recipes rauc-conf"

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# system.conf is a plain configuration file, so bitbake does not expand
# variables inside it. The compatible string has to match the machine the
# bundle was built for, otherwise RAUC refuses to install - substitute it here
# instead of hardcoding a machine name in the layer.
do_install:append() {
    if [ -f ${D}${sysconfdir}/rauc/system.conf ]; then
        sed -i -e "s|@MED_COMPATIBLE@|${DISTRO}-${MACHINE}|g" \
               -e "s|@MED_VERSION@|${DISTRO_VERSION}|g" \
               ${D}${sysconfdir}/rauc/system.conf
    fi
}
