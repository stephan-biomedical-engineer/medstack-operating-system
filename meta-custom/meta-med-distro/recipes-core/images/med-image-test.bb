SUMMARY = "MedOS development / verification image"
DESCRIPTION = "med-image-base with the rootfs left writable and the tooling a \
developer needs to inspect the platform: ssh, gdb/strace, the RAUC and \
journalctl CLIs, cgroup and rpmsg introspection. Never shipped - it exists so \
that the verification steps of the thesis can be run against the same OS \
policy the production image uses."
LICENSE = "MIT"

require recipes-core/images/med-image-base.bb

# Writable rootfs, ssh, debug-tweaks and the verification toolset - the same
# development profile the device images use, so this image and they cannot
# disagree about what "development" means.
require recipes-core/images/med-image-dev.inc

# What this image adds on top of that profile: a developer at a prompt, rather
# than the verification procedure.
IMAGE_FEATURES += " \
    tools-debug \
    empty-root-password \
"

IMAGE_INSTALL:append = " \
    strace \
    ltrace \
    gdb \
    procps \
    htop \
    iproute2 \
    tcpdump \
    e2fsprogs \
    file \
    less \
    vim-tiny \
"
