# MedOS A/B update policy.
#
# meta-rauc ships an (empty) rauc-conf recipe whose only job is to package
# /etc/rauc/system.conf; the integrator is expected to override the file
# through the file search path. Prepending THISDIR/${PN} makes our system.conf
# win without touching meta-rauc's SRC_URI.
#
# NOTE: the append is intentionally "rauc-conf_%" and not "rauc-conf" so it
# matches the versioned recipe (rauc-conf_1.0.bb). Confirm the recipe exists in
# the configured meta-rauc branch with:
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
