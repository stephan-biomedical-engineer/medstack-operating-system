SUMMARY = "MedPlatform infusion pump device profile (simulated mechanism)"
DESCRIPTION = "The second device class built on the platform, and the first \
that acts on the patient: a syringe pump's controller over a simulated \
mechanism. Built from exactly what the tomograph is built from - the same \
base, the same development profile, the same platform packagegroups - plus \
the profile's own applications, so that the package difference between the \
two is the class and nothing else (implementation_plan_infusion_pump.md, M3)."
LICENSE = "MIT"

require recipes-core/images/med-image-base.bb
require recipes-core/images/med-image-dev.inc

# The platform, as the tomograph has it, and the profile's applications: the
# controller (with infusion-ctl) and the simulated mechanism, which is the
# producer the EEG's real-time core plays for the EEG. No packagegroup-med-amp:
# the mechanism runs on Linux in this profile (the co-processor is phase 10).
MED_INFUSION_INSTALL ?= " \
    packagegroup-med-core \
    packagegroup-med-gui \
    infusion-control-service \
    infusion-sim \
"

IMAGE_INSTALL:append = " ${MED_INFUSION_INSTALL}"
