# Deploy U-Boot's own default environment so med-uboot-env-image can build on it.
#
# Why this append exists
# ----------------------
# When U-Boot finds a valid saved environment it *replaces* its built-in one:
# env_import() calls himport_r() without H_NOCLEAR, which clears the hash table
# before importing. There is no merge. So an environment image containing only
# MedPlatform's variables would produce a board with no bootcmd_stm32mp, no
# kernel_addr_r and no console - that is, with the A/B bootcmd present and every
# path it depends on gone. The environment written to the u-boot-env partition
# has to be the vendor's default plus our overrides, and the vendor's default is
# only obtainable from the U-Boot build.
#
# poky's u-boot.inc already generates it - UBOOT_INITIAL_ENV defaults to
# "${PN}-initial-env" and do_compile runs "make u-boot-initial-env" - but it only
# *installs* it, into ${sysconfdir} of the target package. do_deploy does not
# touch it, and ${sysconfdir} is not in SYSROOT_DIRS, so no recipe can reach it
# at build time. Copying it to DEPLOYDIR is the whole content of this file.
#
# Where it lands, and why that is not obvious
# -------------------------------------------
# Not in the top of DEPLOY_DIR_IMAGE. meta-st sets
#
#     do_deploy[sstate-outputdirs] = "${DEPLOY_DIR_IMAGE}${FIP_DIR_UBOOT}"
#
# so everything this task deploys is remapped into the u-boot/ subdirectory
# alongside u-boot-nodtb-*.bin. MED_UBOOT_INITIAL_ENV in conf/layer.conf names
# that path. This cost a build cycle: an empty deploy *root* was read as "the
# append never ran", the append was deleted as redundant, and the next build
# failed for the opposite reason.
#
# Direction of dependency: meta-st-stm32mp is priority 6, below this layer's 7.
# meta-med-bsp appending a recipe from a layer *below* it is the allowed
# direction; what the layer forbids is appending upward, onto meta-med-distro,
# meta-med-framework or meta-med-app. See conf/layer.conf.

do_deploy:append() {
    med_found=""
    for med_env in $(find ${B} -maxdepth 2 -name 'u-boot-initial-env*' -type f 2>/dev/null); do
        install -m 0644 "$med_env" ${DEPLOYDIR}/$(basename "$med_env")
        med_found="yes"
    done

    # A build that quietly deploys nothing here would surface two steps later,
    # in med-uboot-env-image, as a file that does not exist. Name the cause
    # where it happens.
    if [ -z "$med_found" ]; then
        bbfatal "no u-boot-initial-env under ${B}. poky's u-boot.inc generates \
it when UBOOT_INITIAL_ENV is set (it defaults to \\${PN}-initial-env); if \
meta-st-stm32mp has overridden do_compile and dropped that step, \
med-uboot-env-image cannot build the A/B boot environment and \
docs/implementation_plan_uboot_ab.md §4.3.1 needs revisiting."
    fi
}
