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

# Set by the BSP for a machine whose GPU this product actually uses; empty
# everywhere else, including on this same board with the ST EULA not accepted.
# Same hook as MED_AMP_FIRMWARE in packagegroup-med-amp: the driver and the
# vendor EGL userland are board facts, so meta-med-bsp names them and this layer
# does not learn a machine name. Software rendering is what an empty value
# means, and it is a supported state, not a degraded one.
MED_GPU_PACKAGES ?= ""

# Which compositor, and nothing about it. The default is the single-screen one
# every profile has used; an image that wants a desktop names another package
# here, and this recipe never learns what a desktop is. Two compositors in one
# image are two units after the same screen, which is why this is a choice and
# not an addition.
MED_GUI_COMPOSITOR ?= "weston weston-init"

RDEPENDS:${PN} = " \
    ${MED_GUI_COMPOSITOR} \
    qtbase \
    qtbase-plugins \
    qtdeclarative \
    qtdeclarative-qmlplugins \
    qtwayland \
    ttf-dejavu-sans \
    ${MED_GPU_PACKAGES} \
"
