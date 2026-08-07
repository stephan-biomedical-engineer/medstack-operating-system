SUMMARY = "MedFramework runtime (MedPlatform layer 3)"
DESCRIPTION = "Everything an application needs at runtime to use the \
MedFramework APIs: the library itself plus the OS services its \
implementations talk to (journald is part of systemd, the RAUC daemon backs \
MedicalUpdate, cryptsetup backs MedicalStorage's encrypted volume). \
Every MedPlatform device profile installs this packagegroup unchanged - it is \
the shared half of the reuse metric."
LICENSE = "MIT"

PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup

RDEPENDS:${PN} = " \
    med-framework-api \
    dbus \
    rauc \
"
