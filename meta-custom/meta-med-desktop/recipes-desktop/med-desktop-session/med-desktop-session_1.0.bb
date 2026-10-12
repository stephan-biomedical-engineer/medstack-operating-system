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
    file://med-files \
    file://file-manager-settings.conf \
"

S = "${WORKDIR}"

inherit systemd useradd features_check

REQUIRED_DISTRO_FEATURES = "systemd wayland"

# Fixed numbers, from the distro, for the reason given beside them in
# med-os.conf: this user will own files that outlive the slot defining it.
python () {
    for name in ('MED_DESKTOP_UID', 'MED_DESKTOP_GID', 'MED_REVIEW_GID'):
        value = d.getVar(name) or ''
        if not value.isdigit():
            bb.fatal("%s='%s': the desktop identity needs a fixed number, declared by the "
                     "distro (conf/distro/med-os.conf)." % (name, value))
}

# --- The output mode, when a board has to say one ----------------------------
# The same two variables the single-screen compositor reads, answered per
# machine by the board layer and empty where the display chain takes what the
# monitor prefers. This compositor has no configuration for modes, so the
# session sets it once the compositor is up; the recipe never learns a
# connector name, and becomes machine-specific only because the answer is.
MED_WESTON_OUTPUT_NAME ?= ""
MED_WESTON_OUTPUT_MODE ?= ""
PACKAGE_ARCH = "${MACHINE_ARCH}"

USERADD_PACKAGES = "${PN}"
# "wayland" is the group a compositor's socket is opened to, whichever
# compositor it is: a client that is its own system user asks its unit for
# that group and nothing else. The single-screen compositor's package creates
# the same group; this is the other half of the same contract. Its number is
# free to float - it owns a socket in /run, never a file that persists.
#
# "medreview" is the group closed sessions' clinical files are published to.
# On a station the user of the desktop IS the reviewer: a file manager started
# from the task bar runs as that user, and a file manager that cannot open the
# one kind of file the station produces is not one. The group is declared here
# by the distro's number, the same one the reviewer's own package declares it
# by, so either package may be installed first and neither names the other.
# What stays apart is who acquires (and reads the raw record) and who draws the
# live signal; implementation_plan_desktop.md §3 records this as a decision.
GROUPADD_PARAM:${PN} = " \
    --system --gid ${MED_DESKTOP_GID} med-desktop; \
    --system wayland; \
    --system --gid ${MED_REVIEW_GID} medreview \
"
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
    if [ -n "${MED_WESTON_OUTPUT_NAME}" ] && [ -n "${MED_WESTON_OUTPUT_MODE}" ]; then
        printf '\n%s\n%s\n' \
            "# The mode ${MACHINE}'s display chain needs (meta-med-bsp). A refusal is in the journal." \
            "wlr-randr --output ${MED_WESTON_OUTPUT_NAME} --mode ${MED_WESTON_OUTPUT_MODE}Hz" \
            >> ${D}${sysconfdir}/xdg/labwc/autostart
    elif [ -n "${MED_WESTON_OUTPUT_NAME}${MED_WESTON_OUTPUT_MODE}" ]; then
        bbfatal "MED_WESTON_OUTPUT_NAME='${MED_WESTON_OUTPUT_NAME}' and MED_WESTON_OUTPUT_MODE='${MED_WESTON_OUTPUT_MODE}' for ${MACHINE}: a pinned output needs both"
    fi
    install -D -m 0755 ${WORKDIR}/med-files ${D}${bindir}/med-files
    # The file manager reads its "default" profile from here when the user has
    # none, and this user never has one: its home is a runtime directory.
    install -D -m 0644 ${WORKDIR}/file-manager-settings.conf \
        ${D}${sysconfdir}/xdg/pcmanfm-qt/default/settings.conf
}

FILES:${PN} = " \
    ${systemd_system_unitdir}/med-desktop.service \
    ${systemd_system_unitdir}/med-compositor.target \
    ${nonarch_libdir}/medplatform/compositor.env \
    ${sysconfdir}/xdg/labwc \
    ${sysconfdir}/xdg/pcmanfm-qt/default \
    ${bindir}/med-files \
"

# dbus: the session bus the file manager needs to be one program however many
# times its launcher is clicked. qtsvg-plugins: the icon theme is drawn from
# vector files, and without the plugin every icon is an empty square.
RDEPENDS:${PN} = "labwc seatd sfwbar grim wlr-randr pcmanfm-qt dbus qtsvg-plugins"
