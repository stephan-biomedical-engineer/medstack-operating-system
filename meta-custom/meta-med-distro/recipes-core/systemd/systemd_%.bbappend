# MedOS systemd policy: audit-grade journal and a deterministic network config.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI += " \
    file://10-journald-audit.conf \
    file://80-wired.network \
"

do_install:append() {
    install -d ${D}${sysconfdir}/systemd/journald.conf.d
    install -m 0644 ${WORKDIR}/10-journald-audit.conf \
        ${D}${sysconfdir}/systemd/journald.conf.d/10-journald-audit.conf

    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/80-wired.network \
        ${D}${sysconfdir}/systemd/network/80-wired.network

    # The journal is the audit trail: it must survive a power cycle, so the
    # directory has to exist in the image (a read-only rootfs cannot create it
    # at runtime).
    install -d -m 2755 ${D}${localstatedir}/log/journal
}

FILES:${PN} += " \
    ${sysconfdir}/systemd/journald.conf.d \
    ${sysconfdir}/systemd/network \
    ${localstatedir}/log/journal \
"
