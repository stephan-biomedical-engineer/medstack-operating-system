SUMMARY = "MedPlatform EEG update bundle (RAUC)"
DESCRIPTION = "A signed, verifiable A/B update carrying the EEG rootfs. This \
recipe is also the cheapest test of the update path: building it exercises the \
signing certificate, and 'rauc info --keyring' against the produced bundle \
exercises the device's keyring, its codeSigning purpose check and its CRL - \
none of which any image build can validate, and none of which the device \
itself reaches until RAUC can resolve its slots."
LICENSE = "MIT"

inherit bundle

# Must match byte for byte what rauc-conf.bbappend writes into system.conf,
# which is ${DISTRO}-${MACHINE}. The class default is ${MACHINE}-${TARGET_VENDOR}
# ("qemux86-64-med" here), and the mismatch would not fail anything: the bundle
# would build clean, sign clean, and be rejected on the device at install time.
# If either side of this pairing changes, both change.
RAUC_BUNDLE_COMPATIBLE = "${DISTRO}-${MACHINE}"
RAUC_BUNDLE_VERSION = "${DISTRO_VERSION}"

# system.conf declares bundle-formats=-plain, i.e. the device refuses the
# legacy plain format. The class default is empty, which bbwarns and then
# produces a plain bundle anyway - so leaving this unset yields an artefact
# that builds, warns quietly, and is refused at install.
RAUC_BUNDLE_FORMAT = "verity"

RAUC_BUNDLE_SLOTS = "rootfs"
RAUC_SLOT_rootfs = "med-image-eeg"
RAUC_SLOT_rootfs[fstype] = "ext4"

# Signing material. scripts/med-pki.sh generates it under pki/ at the
# repository root; MED_PKI_DIR is anchored to the meta-med-distro layer (see
# its layer.conf) so this resolves both natively and inside the kas container.
#
# NOTE FOR THE DISSERTATION - this is a development compromise, not the model:
# bundle.bbclass signs during do_bundle, by invoking "rauc bundle --key=", so
# the *private signing key is readable by the build*. Inside the container the
# repository is bind-mounted at /repo, which puts pki/ in reach of every task
# that runs there. That is acceptable for the development CA this repository
# ships and is incompatible with the production model scripts/med-pki.sh
# describes, where the root key lives in an HSM. In production, signing is a
# separate, access-controlled step performed on the built artefact - not a
# bitbake task. Say so in the text rather than presenting this as shippable.
RAUC_KEY_FILE = "${MED_PKI_DIR}/signing/med-signing.key.pem"
RAUC_CERT_FILE = "${MED_PKI_DIR}/signing/med-signing.cert.pem"

# A fresh clone has no pki/ - deliberately, since the ability to sign an update
# devices will accept is not repository content. Without this check the failure
# is an opaque openssl error from inside "rauc bundle"; with it, it names the
# script that fixes it. Checked at task time, not parse time, so that building
# anything else in the tree does not require signing material.
python med_check_signing_material() {
    import os

    missing = [v for v in ('RAUC_KEY_FILE', 'RAUC_CERT_FILE')
               if not os.path.exists(d.getVar(v) or '')]
    if missing:
        bb.fatal("no update-signing material: %s not found.\n"
                 "Generate the development CA with 'make pki' "
                 "(scripts/med-pki.sh). Private keys stay out of git by design."
                 % ", ".join(d.getVar(v) for v in missing))
}
do_bundle[prefuncs] += "med_check_signing_material"
