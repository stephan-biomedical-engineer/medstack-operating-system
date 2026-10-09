SUMMARY = "MedPlatform Cortex-M33 producer firmware - the AMP link's far end"
DESCRIPTION = "Bare-metal firmware for the STM32MP257's Cortex-M33. It announces \
an rpmsg-raw channel, answers the application's front-end prescription with a \
control acknowledgement, and emits sample frames in the format med-amp-abi \
defines, read from the converter on SPI6 at each DRDY interrupt and timestamped \
on the M33 at that instant. Also the unit and udev rule that start it on boot and \
give its channel a stable name (phase 4 of docs/implementation_plan_m33_firmware.md)."
HOMEPAGE = "https://github.com/stephan-biomedical-engineer"

# Ours is MIT. ST's OpenAMP glue (files/st/) and the STM32CubeMP2 HAL are
# BSD-3-Clause, CMSIS is Apache-2.0 - the same triple ST's own M33 recipes
# declare, against the same License.md.
LICENSE = "MIT & BSD-3-Clause & Apache-2.0"
LIC_FILES_CHKSUM = " \
    file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302 \
    file://${WORKDIR}/cube/License.md;md5=012a8d78c6f636371ad889eadb15885c \
"

# The Cube tree, at the revision meta-st-stm32mp pins for its own M33 projects
# (mxxprojects-stm32mp2-common.inc). Kept equal on purpose: the HAL this
# firmware is built against should be the one the BSP's examples are built
# against, so that "ST's example loads" (BRINGUP_STM32MP2.md §9.13) remains
# evidence about this firmware's foundations. A BSP bump changes that file;
# this line has to follow it.
SRC_URI = " \
    git://github.com/STMicroelectronics/STM32CubeMP2.git;protocol=https;nobranch=1;name=cube;destsuffix=cube \
    file://CMakeLists.txt;subdir=fw \
    file://src;subdir=fw \
    file://st;subdir=fw \
    file://med-m33-load.sh \
    file://med-m33-firmware.service \
    file://90-med-amp.rules \
"
SRCREV_cube = "2f7258aa45e916777ffb4f6e1b5590f65304378d"

S = "${WORKDIR}/fw"
B = "${WORKDIR}/build"

inherit features_check sign-stm32mp python3native

REQUIRED_MACHINE_FEATURES = "m33copro"
COMPATIBLE_MACHINE = "(stm32mp2common)"
PACKAGE_ARCH = "${MACHINE_ARCH}"

# med-amp-abi is the one thing this layer consumes from the platform: the wire
# format, as a header. Not a copy of it (conf/layer.conf explains why).
DEPENDS += "gcc-arm-none-eabi-native cmake-native med-amp-abi"

# The name Linux loads it by: echo <this> > /sys/class/remoteproc/<m33>/firmware
MED_M33_FW = "med-m33-producer"

# The SoC the signing helper is asked about. It only consults it when
# SIGN_COPRO_ENABLE = 1 (a project key); with the default signature it is
# unused, but it is passed honestly rather than as a placeholder.
MED_M33_SOC = "${@next((s for s in (d.getVar('STM32MP_SOC_NAME') or '').split() if s.startswith('stm32mp25')), 'stm32mp25')}"

MED_ARM_BIN = "${STAGING_DATADIR_NATIVE}/gcc-arm-none-eabi/bin"

# The environment bitbake gives a target recipe describes the APPLICATION
# processor: aarch64 flags, a --sysroot, a target CC. None of it may reach a
# Cortex-M33 build - CMake reads CFLAGS from the environment as its initial
# flags, and an -mcpu=cortex-a35 there is a compile error at best.
med_m33_env() {
    unset CC CXX CPP AS LD AR NM RANLIB OBJCOPY OBJDUMP STRIP
    unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS ASFLAGS
    export PATH="${MED_ARM_BIN}:${STAGING_BINDIR_NATIVE}:$PATH"
}

# A fresh build directory on every configure, made by bitbake: the task runs
# with ${B} as its working directory, so deleting it from inside the task
# leaves cmake with no current directory.
do_configure[cleandirs] = "${B}"
do_configure() {
    med_m33_env
    cmake -S ${S} -B ${B} -G "Unix Makefiles" \
        -DCUBE_DIR=${WORKDIR}/cube \
        -DMED_AMP_ABI_INCLUDE_DIR=${STAGING_INCDIR}/medplatform \
        -DCMAKE_C_FLAGS="-ffile-prefix-map=${WORKDIR}=/usr/src/med-m33-firmware"
}
# The prefix map is not cosmetic. The HAL's assertions embed __FILE__, and
# without it the signed image carries this build host's TMPDIR - QA's
# "buildpaths", and two builds of the same source on two hosts producing two
# different signed firmwares.

do_compile() {
    med_m33_env
    cmake --build ${B}

    # Sign exactly as ST's m33projects does (sign_copro in m33projects.inc):
    # the non-secure load address is read from the ELF's vector table, not
    # assumed.
    nsboot=$(arm-none-eabi-readelf -S ${B}/med_m33_producer_stripped.elf | \
             awk '/isr_vectors/ { print $5 }')
    [ -n "$nsboot" ] || bbfatal "no .isr_vectors section in the firmware ELF"

    rm -f ${B}/${MED_M33_FW}_sign.bin
    (
        export NSBOOTADDR=0x$nsboot
        export SBOOTADDR=0x80000000
        sign_copro_fw_m33 "${MED_M33_SOC}" "${B}/${MED_M33_FW}" \
                          "${B}/med_m33_producer_stripped.elf"
    )

    # sign_copro_fw_m33 reports a failure with bbwarn and carries on. A
    # firmware the OP-TEE will refuse must not become a green build, so the
    # result is checked here: it exists, and it starts with the magic of the
    # OP-TEE remoteproc image format (0x3543A468, little-endian), which is what
    # the ST example that loaded on 2026-10-06 starts with.
    [ -s ${B}/${MED_M33_FW}_sign.bin ] || \
        bbfatal "signing produced no ${MED_M33_FW}_sign.bin"
    magic=$(od -An -tx4 -N4 ${B}/${MED_M33_FW}_sign.bin | tr -d ' ')
    [ "$magic" = "3543a468" ] || \
        bbfatal "${MED_M33_FW}_sign.bin starts with 0x$magic, not the OP-TEE remoteproc magic 0x3543a468"
}

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware
    install -m 0644 ${B}/${MED_M33_FW}_sign.bin ${D}${nonarch_base_libdir}/firmware/

    # Phase 4: started on boot, by udev, and reached by name.
    install -d ${D}${libexecdir} ${D}${systemd_system_unitdir} \
               ${D}${nonarch_base_libdir}/udev/rules.d
    sed -e 's|@MED_M33_FW@|${MED_M33_FW}|g' ${WORKDIR}/med-m33-load.sh \
        > ${D}${libexecdir}/med-m33-load
    chmod 0755 ${D}${libexecdir}/med-m33-load
    sed -e 's|@LIBEXECDIR@|${libexecdir}|g' ${WORKDIR}/med-m33-firmware.service \
        > ${D}${systemd_system_unitdir}/med-m33-firmware.service
    chmod 0644 ${D}${systemd_system_unitdir}/med-m33-firmware.service
    install -m 0644 ${WORKDIR}/90-med-amp.rules ${D}${nonarch_base_libdir}/udev/rules.d/

    # A placeholder that survived substitution is a unit that runs nothing, and
    # it would fail at boot rather than here.
    if grep -rn '@[A-Z_]*@' ${D}${libexecdir}/med-m33-load \
                             ${D}${systemd_system_unitdir}/med-m33-firmware.service; then
        bbfatal "unsubstituted placeholder in the M33 loader or its unit"
    fi
}

# The unsigned ELF is the debugging artefact - symbols for a fault address in
# the trace buffer - and it is deployed, never installed: the M33 in this
# configuration loads only the signed image.
inherit deploy
do_deploy() {
    install -d ${DEPLOYDIR}/med-m33-firmware
    install -m 0644 ${B}/med_m33_producer.elf ${B}/med_m33_producer.map \
                    ${B}/${MED_M33_FW}_sign.bin ${DEPLOYDIR}/med-m33-firmware/
}
addtask deploy after do_compile before do_build

FILES:${PN} = " \
    ${nonarch_base_libdir}/firmware/${MED_M33_FW}_sign.bin \
    ${libexecdir}/med-m33-load \
    ${systemd_system_unitdir}/med-m33-firmware.service \
    ${nonarch_base_libdir}/udev/rules.d/90-med-amp.rules \
"

# The unit is not enabled into any target, and that is why systemd.bbclass is
# not inherited: it has no [Install] section, because the m33 remoteproc device
# wants it (90-med-amp.rules), and an "enable" would be a second way of starting
# it, racing the first.
RDEPENDS:${PN} = "udev"
