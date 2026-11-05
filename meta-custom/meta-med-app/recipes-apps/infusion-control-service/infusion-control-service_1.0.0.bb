SUMMARY = "MedPlatform infusion pump profile - control service"
DESCRIPTION = "The controller of the simulated syringe pump: therapy states, \
dose error reduction against a drug library sealed at build time, alarms, the \
supervision of the mechanism through the framework's actuator and readback \
drivers, and an audit record for every transition. Requirements: \
docs/REQUISITOS_INFUSAO.md."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://CMakeLists.txt;subdir=sources \
    file://src;subdir=sources \
    file://infusion.conf \
    file://infusion-control.service \
"

S = "${WORKDIR}/sources"

# The framework and nothing else (CLAUDE.md rule 6, F5 of make check-layers).
DEPENDS = "med-framework-api"

inherit cmake pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd"

SYSTEMD_SERVICE:${PN} = "infusion-control.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -d ${D}${sysconfdir}/medplatform
    install -m 0640 ${WORKDIR}/infusion.conf ${D}${sysconfdir}/medplatform/infusion.conf
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/infusion-control.service ${D}${systemd_system_unitdir}/
}

# The drug library is sealed by the same rule as the EEG's calibration
# (CLAUDE.md rule 8): the service refuses a store without a matching sidecar,
# and the sidecar is written here, after the file is final. A value followed
# by a comment is refused, because the comment would become part of the value
# - a hard limit of "50  # mL/h" is not a number, and the service would refuse
# to start on the device instead of the build refusing here.
python do_seal_configuration() {
    import os

    store = os.path.join(d.getVar('D') + d.getVar('sysconfdir'), 'medplatform', 'infusion.conf')
    if not os.path.exists(store):
        bb.fatal("infusion.conf was not installed; nothing to seal")
    with open(store) as handle:
        lines = handle.readlines()
    inline = [(n, l.rstrip()) for n, l in enumerate(lines, 1)
              if not l.lstrip().startswith('#') and '=' in l
              and '#' in l.split('=', 1)[1]]
    if inline:
        bb.fatal("infusion.conf has an inline comment in a value, which becomes "
                 "part of the value:\n  " + "\n  ".join(
                     "line %d: %s" % (n, l) for n, l in inline) +
                 "\nPut the comment on its own line.")
    digest = bb.utils.sha256_file(store)
    sidecar = store + '.sha256'
    with open(sidecar, 'w') as handle:
        handle.write(digest + '\n')
    os.chmod(sidecar, 0o640)
    bb.note("sealed %s with sha256 %s" % (store, digest))
}
addtask seal_configuration after do_install before do_package
do_seal_configuration[fakeroot] = "1"
do_seal_configuration[depends] += "virtual/fakeroot-native:do_populate_sysroot"

FILES:${PN} += " \
    ${sysconfdir}/medplatform \
    ${systemd_system_unitdir}/infusion-control.service \
"
