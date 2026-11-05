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
"

S = "${WORKDIR}/sources"

# The framework and nothing else (CLAUDE.md rule 6, F5 of make check-layers).
DEPENDS = "med-framework-api"

inherit cmake pkgconfig features_check

REQUIRED_DISTRO_FEATURES = "systemd"

# Its unit, the user it runs as and the image that installs it are phase 6 of
# implementation_plan_infusion_pump.md; until then the recipe builds the
# program and nothing installs it.
