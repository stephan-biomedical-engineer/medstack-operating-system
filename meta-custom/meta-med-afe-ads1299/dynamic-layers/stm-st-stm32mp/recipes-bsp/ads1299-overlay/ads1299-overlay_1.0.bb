SUMMARY = "Devicetree overlay for the ADS1299 on the STM32MP257F-DK hat connector"
DESCRIPTION = "Enables SPI6 on the Cortex-A35 side and declares the ADS1299 \
with DRDY as a real interrupt - the hat link with the converter on Linux. Not \
the product topology, where SPI6 belongs to the Cortex-M33 through the RIF."

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit devicetree

COMPATIBLE_MACHINE = "stm32mp25-disco"

SRC_URI = "file://stm32mp257f-dk-ads1299-hat.dtso.in"

DT_FILES = "stm32mp257f-dk-ads1299-hat.dtso"

# ---------------------------------------------------------------------------
# The three pins this overlay cannot know
# ---------------------------------------------------------------------------
# Written as full phandle+cell expressions rather than as pin numbers, because
# the bank is as unknown as the offset:
#
#   MED_AFE_CS_GPIO    = "&gpiof 3 GPIO_ACTIVE_LOW"
#   MED_AFE_DRDY_GPIO  = "&gpioa 5 0"      (interrupts-extended: phandle + pin)
#   MED_AFE_RESET_GPIO = "&gpiob 2 GPIO_ACTIVE_LOW"
#
# They are declared empty, and the build stops if they still are. That is the
# whole point of this recipe existing before the board does: an overlay with a
# guessed pin compiles, installs, boots and produces a converter that never
# raises DRDY - a silence indistinguishable from a dead chip, three layers away
# from the cause. A build that refuses costs nothing and says exactly what is
# missing.
#
# The DRDY pin has a second constraint that has to be honoured while the
# schematic is open, not after: on the product topology the Cortex-M33 owns
# that interrupt, so the pin must belong to a GPIO bank whose RIF can assign it
# to the M33's CID, alongside SPI6 (rifsc 27). Choosing it by connector
# position alone is how a board revision gets spent.
MED_AFE_CS_GPIO ?= ""
MED_AFE_DRDY_GPIO ?= ""
MED_AFE_RESET_GPIO ?= ""

python do_check_pins() {
    unanswered = [name for name in ('MED_AFE_CS_GPIO', 'MED_AFE_DRDY_GPIO',
                                    'MED_AFE_RESET_GPIO')
                  if not (d.getVar(name) or '').strip()]
    if unanswered:
        bb.fatal(
            "%s not declared. The ADS1299 hat overlay needs the chip select, "
            "DRDY and RESET pins of the board it is going on, and there is no "
            "safe default: a wrong pin compiles and boots. Declare them in the "
            "KAS project file as full devicetree expressions, e.g.\n"
            "    MED_AFE_DRDY_GPIO = \"&gpioa 5 0\"\n"
            "See docs/implementation_plan_ads1299.md §4.1 and §4.2."
            % ', '.join(unanswered))
}
addtask check_pins before do_configure

do_configure:prepend() {
    sed -e "s|@MED_AFE_CS_GPIO@|${MED_AFE_CS_GPIO}|g" \
        -e "s|@MED_AFE_DRDY_GPIO@|${MED_AFE_DRDY_GPIO}|g" \
        -e "s|@MED_AFE_RESET_GPIO@|${MED_AFE_RESET_GPIO}|g" \
        ${WORKDIR}/stm32mp257f-dk-ads1299-hat.dtso.in \
        > ${S}/stm32mp257f-dk-ads1299-hat.dtso
}

# Not in MED_AFE_INSTALL. Compiling the overlay is not the same as applying it:
# nothing in the boot path calls "fdt apply" yet, and that line belongs to the
# bootcmd meta-med-bsp owns, not here. Build it deliberately -
#
#   kas shell kas/project-eeg-stm32mp2.yml -c "bitbake ads1299-overlay"
#
# - which is exactly what checks the syntax and the &spi6 / &spi6_pins_a symbol
# references before anyone is standing at a bench.
