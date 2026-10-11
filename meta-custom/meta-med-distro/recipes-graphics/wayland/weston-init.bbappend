# The operator display never blanks, comes up with or without a monitor, and a
# board may pin its output mode.
#
# The file is weston-init.bbappend and not weston-init_%.bbappend because
# oe-core ships weston-init.bb unversioned - the same trap as rauc-conf.bb, where
# a "_%" bbappend matches nothing and is a hard parse error.
#
# --- The screen stays on -----------------------------------------------------
# Weston blanks an output after 300 s without input. On a device with no input
# device attached that timer is never reset: the compositor is up, the unit is
# active, and the output is off. Measured on the board 2026-08-27
# (BRINGUP_HMI_STM32MP2.md §4), where the CRTC went from enable=0 to active=1
# with this one setting and nothing else changed.
#
# "An operator display does not turn itself off" is device policy. It holds on
# every board, names no machine, and so it is set here and not in meta-med-bsp.
#
# oe-core's recipe already knows how to write it - PACKAGECONFIG
# "no-idle-timeout" adds idle-time=0 under [core] - so this layer asks for that
# rather than carrying a weston.ini of its own to keep in step with upstream's.
PACKAGECONFIG:append = " no-idle-timeout"

# --- The output mode, when a board has to say one ----------------------------
# Empty on a machine whose display chain takes whatever the monitor prefers,
# which is the normal case. A board that needs a particular connector driven in
# a particular mode declares both in meta-med-bsp, keyed by machine; this
# recipe never learns a connector name. Same shape as MED_GPU_PACKAGES and
# MED_BSP_INSTALL.
MED_WESTON_OUTPUT_NAME ?= ""
MED_WESTON_OUTPUT_MODE ?= ""

do_install:append() {
    ini=${D}${sysconfdir}/xdg/weston/weston.ini

    # Asking upstream's recipe for a behaviour is an assumption about third
    # party code, so the produced file is read rather than trusted. If oe-core
    # renames or drops the option, the PACKAGECONFIG above becomes a no-op
    # that fails nothing and blanks the screen five minutes into a session.
    if ! grep -q '^idle-time=0$' "$ini"; then
        bbfatal "weston.ini carries no idle-time=0: the no-idle-timeout PACKAGECONFIG of weston-init no longer does what this layer relies on"
    fi

    # --- The display stack does not need a monitor to start -----------------
    # Weston's default is to exit when no output is connected at startup, and
    # weston.service does not restart. A device powered on before its monitor
    # therefore has no compositor and no operator display until the next
    # reboot, and plugging the monitor in changes nothing. Measured on the
    # board 2026-10-11: with the GPU and the mode already right, the renderer
    # came up, "connector 32 is disconnected", exit 1 at four seconds.
    #
    # With "none" the compositor starts with zero outputs and enables one when
    # a monitor appears; the HMI creates a placeholder screen meanwhile. The
    # order in which a display and a device are switched on is not something
    # this OS gets to assume, on any board.
    sed -i -e "/^\[core\]/a require-outputs=none" "$ini"
    if ! grep -q '^require-outputs=none$' "$ini"; then
        bbfatal "weston.ini has no [core] section to carry require-outputs=none"
    fi

    # --- No panel ---------------------------------------------------------------
    # The stock shell draws a panel with a clock and, when no launcher is
    # configured, a launcher for a terminal - which this image carries, because
    # it ships inside the compositor's own package. That is a shell one click
    # away from the operator's display. And the clock beside it shows the time
    # of a board with no battery-backed clock, which is worse than showing none.
    #
    # The applications on this device are started by systemd units, each as its
    # own user; nothing here is chosen from a menu, so the panel has nothing
    # true to offer.
    #
    # And a plain dark background: it is what shows for the moment between the
    # compositor and the first window, and the stock one is a test pattern.
    printf '\n[shell]\npanel-position=none\nbackground-color=0xff0b0f14\n' >> "$ini"

    if [ -n "${MED_WESTON_OUTPUT_NAME}" ] && [ -n "${MED_WESTON_OUTPUT_MODE}" ]; then
        printf '\n[output]\nname=%s\nmode=%s\n' \
            "${MED_WESTON_OUTPUT_NAME}" "${MED_WESTON_OUTPUT_MODE}" >> "$ini"
    elif [ -n "${MED_WESTON_OUTPUT_NAME}${MED_WESTON_OUTPUT_MODE}" ]; then
        bbfatal "MED_WESTON_OUTPUT_NAME='${MED_WESTON_OUTPUT_NAME}' and MED_WESTON_OUTPUT_MODE='${MED_WESTON_OUTPUT_MODE}' for ${MACHINE}: a pinned output needs both, and half of one pins nothing"
    fi
}
