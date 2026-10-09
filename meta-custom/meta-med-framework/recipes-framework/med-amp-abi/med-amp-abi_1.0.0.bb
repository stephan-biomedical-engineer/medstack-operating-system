SUMMARY = "MedFramework - wire format of the AMP link (header only)"
DESCRIPTION = "med_amp_abi.h: the frame header, control message and control \
acknowledgement that cross the boundary between Linux and the firmware on the \
real-time core, with the size and offset of every field asserted at compile \
time. C99 and freestanding, so that a bare-metal producer can include the same \
definition the framework does."
HOMEPAGE = "https://github.com/stephan-biomedical-engineer"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Its own recipe, and not a file inside med-framework-api, because it has two
# consumers built by two different toolchains: libmedframework, for the
# application processor, and the co-processor firmware, which has no use for a
# C++ library and must not have to build one for aarch64 to obtain a header.
# One definition of the format, installed once, consumed by both - the
# alternative was two copies, with the guarantee that they diverge one day
# without anything failing (implementation_plan_m33_firmware.md §5.1).
#
# It is the one thing outside the platform's four layers is allowed to consume
# from them: an adjunct layer's producer firmware depends on this recipe, the
# way a driver depends on the ABI of the kernel it plugs into. It is a format,
# not code - there is nothing here to call.

SRC_URI = "file://med_amp_abi.h"
S = "${WORKDIR}"

inherit allarch

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${includedir}/medplatform
    install -m 0644 ${S}/med_amp_abi.h ${D}${includedir}/medplatform/
}

# A header and nothing else: the main package would be empty, and is allowed
# to be, so that RDEPENDS on the -dev package resolves without inventing a
# runtime component that does not exist.
ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev = "${includedir}/medplatform/med_amp_abi.h"
