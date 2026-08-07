SUMMARY = "MedOS development / verification image"
DESCRIPTION = "med-image-base with the rootfs left writable and the tooling a \
developer needs to inspect the platform: ssh, gdb/strace, the RAUC and \
journalctl CLIs, cgroup and rpmsg introspection. Never shipped - it exists so \
that the verification steps of the thesis can be run against the same OS \
policy the production image uses."
LICENSE = "MIT"

require recipes-core/images/med-image-base.bb

# The one deliberate deviation from the production policy, and the reason this
# is a separate recipe instead of a flag on med-image-prod.
MED_ROOTFS_FEATURES = ""

IMAGE_FEATURES += " \
    ssh-server-openssh \
    debug-tweaks \
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
