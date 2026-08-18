# MedOS runs its compositor unprivileged, and that needs a seat provider.
#
# weston.service (from weston-init, oe-core) declares User=weston / Group=weston,
# so weston cannot open /dev/dri/card0 or the input devices on its own. It asks
# libseat, which tries its backends in order: seatd, then logind, then builtin.
# The builtin backend embeds seat management in the process itself and therefore
# needs root - so for an unprivileged compositor it is not a fallback, it is a
# failure. Measured on the board 2026-08-18: weston exits 1 after 73 ms with
# "Could not open tty0 to update VT: Permission denied", then "no drm device
# found", then "fatal: failed to create compositor backend".
#
# oe-core's seatd recipe installs the daemon and libseat and - under systemd -
# no unit at all: it inherits update-rc.d and then sets INHIBIT_UPDATERCD_BBCLASS
# when the init manager is systemd, so the SysV script is the only thing it ever
# ships. On a systemd image the daemon is present and never runs. Upstream's
# intended path there is logind, and libseat here does carry the logind backend
# (verified by extracting the package: backend_logind_from_libseat_backend and
# the sd_bus symbols are in libseat.so.1). Whether logind grants weston's PAM
# session an *active seat* on this board was not determined.
#
# Running seatd settles that question rather than answering it, and does so in
# the direction this platform should want anyway: libseat tries seatd first, so
# with the daemon up the logind path is not consulted. For a medical device that
# is the better dependency - a 700-line daemon with one job, instead of user
# session semantics that exist to serve interactive multi-user login, which this
# device does not have.
#
# This is OS policy, not a board fact: it is true of every MedOS machine with a
# compositor, names no machine and no device node. It belongs here and not in
# meta-med-bsp.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://seatd.service"

inherit systemd

SYSTEMD_SERVICE:${PN} = "seatd.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/seatd.service ${D}${systemd_system_unitdir}/seatd.service
}

FILES:${PN} += "${systemd_system_unitdir}/seatd.service"
