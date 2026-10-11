SUMMARY = "MedPlatform station desktop: the compositor's unit, identity and configuration"
DESCRIPTION = "Runs the station's compositor as a system service under its \
own unprivileged user, and tells the operator display where that compositor \
is. This is what an image installs in place of the single-screen compositor \
when it wants a desktop: the choice is one variable, MED_GUI_COMPOSITOR, and \
no recipe in the four platform layers names what is in here."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://med-desktop.service \
    file://med-compositor.target \
    file://rc.xml \
    file://autostart \
"

S = "${WORKDIR}"

inherit systemd useradd features_check

REQUIRED_DISTRO_FEATURES = "systemd wayland"

# Fixed numbers, from the distro, for the reason given beside them in
# med-os.conf: this user will own files that outlive the slot defining it.
python () {
    for name in ('MED_DESKTOP_UID', 'MED_DESKTOP_GID'):
        value = d.getVar(name) or ''
        if not value.isdigit():
            bb.fatal("%s='%s': the desktop identity needs a fixed number, declared by the "
                     "distro (conf/distro/med-os.conf)." % (name, value))
}

USERADD_PACKAGES = "${PN}"
# "wayland" is the group a compositor's socket is opened to, whichever
# compositor it is: a client that is its own system user asks its unit for
# that group and nothing else. The single-screen compositor's package creates
# the same group; this is the other half of the same contract. Its number is
# free to float - it owns a socket in /run, never a file that persists.
GROUPADD_PARAM:${PN} = "--system --gid ${MED_DESKTOP_GID} med-desktop; --system wayland"
USERADD_PARAM:${PN} = "--system --uid ${MED_DESKTOP_UID} --gid med-desktop \
    --no-create-home --home-dir /nonexistent --shell /sbin/nologin med-desktop"

SYSTEMD_SERVICE:${PN} = "med-desktop.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -D -m 0644 ${WORKDIR}/med-desktop.service \
        ${D}${systemd_system_unitdir}/med-desktop.service
    # What every compositor package ships, under the same two paths: the
    # target a client's unit waits for, and where a client connects. Two
    # compositors in one image collide on these files and the image does not
    # build, which is the intended way to find out.
    install -D -m 0644 ${WORKDIR}/med-compositor.target \
        ${D}${systemd_system_unitdir}/med-compositor.target
    install -d ${D}${nonarch_libdir}/medplatform
    printf '%s\n' 'WAYLAND_DISPLAY=/run/med-desktop/wayland-0' \
        > ${D}${nonarch_libdir}/medplatform/compositor.env
    install -D -m 0644 ${WORKDIR}/rc.xml ${D}${sysconfdir}/xdg/labwc/rc.xml
    install -D -m 0644 ${WORKDIR}/autostart ${D}${sysconfdir}/xdg/labwc/autostart
}

FILES:${PN} = " \
    ${systemd_system_unitdir}/med-desktop.service \
    ${systemd_system_unitdir}/med-compositor.target \
    ${nonarch_libdir}/medplatform/compositor.env \
    ${sysconfdir}/xdg/labwc \
"

RDEPENDS:${PN} = "labwc seatd sfwbar grim"
