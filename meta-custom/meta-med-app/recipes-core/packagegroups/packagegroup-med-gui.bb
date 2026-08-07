SUMMARY = "MedPlatform HMI graphics stack"
DESCRIPTION = "Wayland compositor and Qt runtime for device profiles with an \
operator display. Contains no application: the graphics stack is the reusable \
part, the HMI itself belongs to the device profile that owns it. That split is \
what lets the tomograph profile reuse this packagegroup byte for byte while \
shipping a completely different interface."
LICENSE = "MIT"

PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup features_check

REQUIRED_DISTRO_FEATURES = "wayland opengl"

RDEPENDS:${PN} = " \
    weston \
    weston-init \
    qtbase \
    qtbase-plugins \
    qtdeclarative \
    qtdeclarative-qmlplugins \
    qtwayland \
    ttf-dejavu-sans \
"
