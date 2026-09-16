SUMMARY = "Stable device naming and permissions for the ADS1299 front-end"
DESCRIPTION = "udev rules that give the ADS1299's IIO character device a name \
that identifies the converter rather than its probe order, and the tightest \
permissions its single consumer can work with."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://70-med-afe-ads1299.rules"

S = "${WORKDIR}"

inherit allarch

do_install() {
    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/70-med-afe-ads1299.rules \
        ${D}${nonarch_base_libdir}/udev/rules.d/70-med-afe-ads1299.rules
}

FILES:${PN} = "${nonarch_base_libdir}/udev/rules.d"

RDEPENDS:${PN} = "udev"
