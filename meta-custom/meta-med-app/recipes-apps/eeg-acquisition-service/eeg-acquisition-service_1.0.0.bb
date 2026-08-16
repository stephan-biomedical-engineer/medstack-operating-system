SUMMARY = "MedPlatform EEG acquisition service (MedApp reference application)"
DESCRIPTION = "Data daemon of the EEG proof of concept. Reads sample frames \
from the acquisition front-end, persists them to the encrypted record volume, \
publishes them to the HMI and keeps the audit trail - all through MedFramework \
APIs, with no direct use of the OS or the BSP. Retargeting it to different \
hardware is a configuration change (MED_EEG_DRIVER), not a code change."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Source tree in a subdirectory, not directly in WORKDIR - see the same note in
# med-framework-api_1.0.0.bb: S = ${WORKDIR} puts PKGD inside S and the
# debug-source hardlinks of do_package make pseudo abort on any task re-run.
# eeg.conf and the unit file stay in WORKDIR because do_install reads them from
# there, and they are not part of the compiled tree.
SRC_URI = " \
    file://CMakeLists.txt;subdir=sources \
    file://src;subdir=sources \
    file://eeg.conf \
    file://eeg-acquisition.service \
"

S = "${WORKDIR}/sources"

DEPENDS = "med-framework-api"

inherit cmake pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd"

SYSTEMD_SERVICE:${PN} = "eeg-acquisition.service"
SYSTEMD_AUTO_ENABLE = "enable"

# The two knobs that make the same application binary serve both targets.
# Set from the KAS project file:
#   project-eeg-qemu.yml      -> simulated front-end, no LUKS volume
#   project-eeg-stm32mp2.yml  -> rpmsg front-end on the Cortex-M4, encrypted /data
MED_EEG_DRIVER ?= "simulated"
MED_EEG_REQUIRE_ENCRYPTION ?= "true"

do_install:append() {
    install -d ${D}${sysconfdir}/medplatform
    sed -e "s|@MED_EEG_DRIVER@|${MED_EEG_DRIVER}|g" \
        -e "s|@MED_EEG_REQUIRE_ENCRYPTION@|${MED_EEG_REQUIRE_ENCRYPTION}|g" \
        ${WORKDIR}/eeg.conf > ${D}${sysconfdir}/medplatform/eeg.conf
    chmod 0640 ${D}${sysconfdir}/medplatform/eeg.conf

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/eeg-acquisition.service \
        ${D}${systemd_system_unitdir}/eeg-acquisition.service
}

# MedicalConfiguration refuses to load a store it cannot verify, so the
# integrity sidecar has to be produced by whoever produced the store - here,
# the build. Doing it in Python rather than shelling out to sha256sum keeps the
# task independent of what happens to be in HOSTTOOLS.
python do_seal_configuration() {
    import os

    config = os.path.join(d.getVar('D') + d.getVar('sysconfdir'), 'medplatform', 'eeg.conf')
    if not os.path.exists(config):
        bb.fatal("eeg.conf was not installed; nothing to seal")

    digest = bb.utils.sha256_file(config)
    sidecar = config + '.sha256'
    with open(sidecar, 'w') as handle:
        handle.write(digest + '\n')
    os.chmod(sidecar, 0o640)
    bb.note("sealed %s with sha256 %s" % (config, digest))
}
addtask seal_configuration after do_install before do_package

# The task writes into ${D}, so it has to run under pseudo like do_install
# does. base.bbclass grants that to do_install only; a task added with addtask
# inherits nothing, and without these two flags the sidecar is created with the
# build user's uid instead of root:root. QA catches it as
# [host-user-contaminated], and it would ship a device whose configuration seal
# is owned by an unprivileged account - the one file that must not be.
do_seal_configuration[fakeroot] = "1"
do_seal_configuration[depends] += "virtual/fakeroot-native:do_populate_sysroot"

FILES:${PN} += " \
    ${sysconfdir}/medplatform \
    ${systemd_system_unitdir}/eeg-acquisition.service \
"
