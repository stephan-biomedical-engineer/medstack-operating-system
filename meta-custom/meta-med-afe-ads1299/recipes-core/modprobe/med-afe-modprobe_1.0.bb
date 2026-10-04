SUMMARY = "Module parameters for the ADS1299 front-end's USB-SPI bridge"
DESCRIPTION = "The chip select the AFE board wires the converter to, given to \
the bridge driver as a module option. Installed only on the usb link."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://med-afe-bridge.conf"

S = "${WORKDIR}"

inherit allarch

do_install() {
    install -d ${D}${sysconfdir}/modprobe.d
    install -m 0644 ${WORKDIR}/med-afe-bridge.conf \
        ${D}${sysconfdir}/modprobe.d/med-afe-bridge.conf
}

FILES:${PN} = "${sysconfdir}/modprobe.d"
