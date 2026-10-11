SUMMARY = "MedPlatform record review: the identity and the unit a reviewing program runs under"
DESCRIPTION = "Creates the med-review user and the medreview group, and ships \
the unit that runs the reviewing program as them. Deliberately not part of \
the program's own recipe: that one packages third-party software unmodified, \
and this one is the decision about what that software is allowed to reach - \
the clinical files of closed sessions, read-only, and nothing else on the \
device."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://med-review.service \
    file://med-review-request.path \
    file://med-review-request.service \
    file://eeg-hmi-review.conf \
"

S = "${WORKDIR}"

inherit systemd useradd features_check

REQUIRED_DISTRO_FEATURES = "systemd wayland"

# The numbers come from the distro (conf/distro/med-os.conf) and never from
# here: the group owns files on /data, which outlives the slot that defines it.
python () {
    for name in ('MED_REVIEW_UID', 'MED_REVIEW_GID'):
        value = d.getVar(name) or ''
        if not value.isdigit():
            bb.fatal("%s='%s': the reviewing identity needs a fixed number, declared by the "
                     "distro. Without one its id follows install order, and the clinical "
                     "files on /data change owner at the next update." % (name, value))
}

USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "--system --gid ${MED_REVIEW_GID} medreview"
# No password, no shell, no home: nobody logs in as this user. The unit is the
# only thing that becomes it.
USERADD_PARAM:${PN} = "--system --uid ${MED_REVIEW_UID} --gid medreview \
    --no-create-home --home-dir /nonexistent --shell /sbin/nologin med-review"

# The path is what is enabled. The reviewer itself has no [Install] section and
# starts only when the operator display asks for it.
SYSTEMD_SERVICE:${PN} = "med-review-request.path"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${systemd_system_unitdir}/eeg-hmi.service.d
    install -m 0644 ${WORKDIR}/med-review.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${WORKDIR}/med-review-request.path ${D}${systemd_system_unitdir}/
    install -m 0644 ${WORKDIR}/med-review-request.service ${D}${systemd_system_unitdir}/
    # The one place this package reaches into the display's unit: where the
    # display leaves its request, and leave to write there. The spool's name
    # appears in three files, all of this recipe - the path unit that watches
    # it, the unit that empties it, and this drop-in.
    install -m 0644 ${WORKDIR}/eeg-hmi-review.conf \
        ${D}${systemd_system_unitdir}/eeg-hmi.service.d/review.conf
}

FILES:${PN} = " \
    ${systemd_system_unitdir}/med-review.service \
    ${systemd_system_unitdir}/med-review-request.path \
    ${systemd_system_unitdir}/med-review-request.service \
    ${systemd_system_unitdir}/eeg-hmi.service.d/review.conf \
"

RDEPENDS:${PN} = "edfbrowser"
