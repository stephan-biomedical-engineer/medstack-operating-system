SUMMARY = "MedOS production image (read-only rootfs, A/B, no shell access)"
DESCRIPTION = "The shippable form of the MedOS base: immutable rootfs, no \
debug features, no remote login, and a WIC image carrying the full A/B GPT \
layout so it can be written to a device with bmaptool. Device profiles derive \
their production variants from this recipe."
LICENSE = "MIT"

require recipes-core/images/med-image-base.bb

# Inherited from med-image-base, restated because it is the defining property
# of this image and must not be lost to an override further down.
MED_ROOTFS_FEATURES = "read-only-rootfs"

# Strip anything that would weaken the deployed device, including features a
# developer's local.conf may have added through EXTRA_IMAGE_FEATURES.
IMAGE_FEATURES:remove = "debug-tweaks tools-debug tools-profile dbg-pkgs \
                         empty-root-password allow-empty-password \
                         allow-root-login post-install-logging"

# The full disk image (ESP + med-root-a + med-root-b + med-data) now comes from
# med-image-base.bb, because the A/B layout is a MedOS property rather than a
# production-only one - a development image that cannot exercise the update
# path is not much use. WKS_FILE comes from MED_WKS_FILE in med-os.conf.
#
# Note that this recipe requires only the base, never med-image-dev.inc. That
# is what keeps a writable rootfs, ssh, debug-tweaks and the introspection
# tooling out of it: production differs from development by not opting in, not
# by remembering to opt out.

# Nothing gets installed here that is not already in the base: a production
# image differs from the base by what it *removes*, which keeps the two in
# lockstep and makes the diff between them auditable.

# --- Service access (ACE-03 of docs/CONFORMIDADE.md).
#
# IEC 81001-5-1 5.3.2 g)-h) lets a product keep an access path it documents
# and protects instead of removing it. A production MedOS image keeps one: no
# account has a usable password (root's is "*"), the stock sshd is masked, and
# the only listener is the one a signed grant opens for a bounded time, for one
# service user and one key, with every step audited. The mechanism audits
# through MedicalLogger, so its package lives in meta-med-framework; this
# recipe names the variable and a profile's production image fills it. Empty
# means no service access at all, which fails safe.
MED_SERVICE_ACCESS ?= ""
IMAGE_INSTALL:append = " ${MED_SERVICE_ACCESS}"

# The stock sshd, if anything pulls it in, never listens here: openssh-sshd
# enables sshd.socket with a configuration that permits root, and masking it
# leaves the grant's own listener (med-service-sshd.socket) as the only one.
med_mask_stock_sshd() {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/sockets.target.wants/sshd.socket \
          ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/multi-user.target.wants/sshd.service
    for unit in sshd.socket sshd.service sshd@.service; do
        ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/$unit
    done
}
ROOTFS_POSTPROCESS_COMMAND += "med_mask_stock_sshd; "
