SUMMARY = "MedPlatform display contract answered by the host's compositor"
DESCRIPTION = "The third package that fulfils the compositor contract every \
operator display unit relies on (med-compositor.target and compositor.env), \
next to weston-init and med-desktop-session. It ships no compositor: when the \
platform's userspace runs as a container on a desktop Linux host, the display \
is the host's, and its Wayland socket is bound into the container at a fixed \
path. Selected by MED_GUI_COMPOSITOR = \"med-host-display\"."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://med-compositor.target \
    file://eeg-hmi-close-ends-station.conf \
"

inherit useradd allarch

# "wayland" is the group a compositor's socket is opened to, whichever
# compositor it is (see med-desktop-session). Here no package in the image
# owns the socket, but a client unit that asks for the group
# (SupplementaryGroups=wayland, med-review.service) fails to start when the
# group does not exist, so the contract still has to declare it.
USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "--system wayland"

do_install() {
    # The same two paths every compositor package ships. Two compositors in one
    # image collide on these files and the image does not build, which is how
    # an image with weston AND the host's display is refused.
    install -D -m 0644 ${WORKDIR}/med-compositor.target \
        ${D}${systemd_system_unitdir}/med-compositor.target
    # Beyond where to connect, one fact about this display: a desktop PC has a
    # menu that reopens a closed window (the station's icon), so the operator
    # display may offer a Close. Without it the HMI offers none.
    install -d ${D}${nonarch_libdir}/medplatform
    printf '%s\n' \
        'WAYLAND_DISPLAY=/run/host-display/wayland-0' \
        'MED_WINDOW_CLOSABLE=1' \
        > ${D}${nonarch_libdir}/medplatform/compositor.env

    # And what that Close means here: the end of the station, not of a window.
    install -D -m 0644 ${WORKDIR}/eeg-hmi-close-ends-station.conf \
        ${D}${systemd_system_unitdir}/eeg-hmi.service.d/close-ends-station.conf
}

FILES:${PN} = " \
    ${systemd_system_unitdir}/med-compositor.target \
    ${systemd_system_unitdir}/eeg-hmi.service.d/close-ends-station.conf \
    ${nonarch_libdir}/medplatform/compositor.env \
"
