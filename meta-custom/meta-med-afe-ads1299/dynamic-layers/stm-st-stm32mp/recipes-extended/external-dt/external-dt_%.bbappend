# The resource-isolation half of the "amp" link: the OP-TEE devicetree that
# decides which core owns SPI6, the hat's GPIOs and DRDY's EXTI line.
#
# Only for MED_EEG_LINK = "amp". The same SPI6 belongs to Linux on the "spi"
# link, and the "usb" and "simulated" links use neither - so, like the kernel
# modules this layer installs, the patch follows the link and not the machine.
#
# Note what applying it costs: the OP-TEE lives in the FIP, and no RAUC bundle
# updates the FIP (only the rootfs slots are A/B). A change here reaches a
# board by writing fip-a, or by flashing the card - never over the air.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

python () {
    if (d.getVar('MED_EEG_LINK') or '') == 'amp':
        d.appendVar('SRC_URI', ' file://0001-optee-dk-give-the-AFE-hat-SPI6-to-the-Cortex-M33.patch')
}
