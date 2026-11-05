SUMMARY = "MedPlatform infusion pump profile - simulated mechanism"
DESCRIPTION = "The producer process that stands in for a syringe pump's \
mechanism: a lead screw, an independent encoder, a first-order line pressure \
and an air detector, behind the framework's actuator and readback protocol, \
with a dead-man timer on its own side. A model with test-chosen parameters, \
never a statement about a real pump (docs/REQUISITOS_INFUSAO.md)."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://CMakeLists.txt;subdir=sources \
    file://src;subdir=sources \
    file://infusion-sim.service \
"

S = "${WORKDIR}/sources"

# The framework and nothing else (CLAUDE.md rule 6, F5 of make check-layers).
DEPENDS = "med-framework-api"

inherit cmake pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd"

SYSTEMD_SERVICE:${PN} = "infusion-sim.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/infusion-sim.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} += "${systemd_system_unitdir}/infusion-sim.service"

# The image that installs it is phase 6 of implementation_plan_infusion_pump.md.
