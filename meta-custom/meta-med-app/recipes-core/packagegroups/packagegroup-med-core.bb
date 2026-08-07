SUMMARY = "MedPlatform common application runtime"
DESCRIPTION = "The runtime every MedPlatform device profile installs, whatever \
class of device it is: the MedFramework library and the OS services its \
implementations depend on. An EEG, an infusion pump and a tomograph differ in \
what they add on top of this, never in this."
LICENSE = "MIT"

PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup

RDEPENDS:${PN} = " \
    packagegroup-med-framework \
"
