# MedOS A/B update policy.
#
# meta-rauc ships an (empty) rauc-conf recipe whose only job is to package
# /etc/rauc/system.conf; the integrator is expected to override the file
# through the file search path. Prepending THISDIR/${PN} makes our system.conf
# win without touching meta-rauc's SRC_URI.
#
# NOTE: the filename must be "rauc-conf.bbappend", NOT "rauc-conf_%.bbappend".
# meta-rauc/scarthgap ships the recipe unversioned as rauc-conf.bb, and bitbake
# matches appends against the recipe *filename*: the "_%" form requires an
# underscore in the .bb name, so it matches nothing here and bitbake fails hard
# with "No recipes in default available for: .../rauc-conf_%.bbappend".
# Re-check after any meta-rauc bump with:
#   kas shell kas/kas-base.yml -c "bitbake-layers show-recipes rauc-conf"

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# The verification keyring. meta-rauc defaults RAUC_KEYRING_FILE to its own
# ca.cert.pem, which is not a certificate at all - it is 358 bytes of comments
# telling the integrator to replace it, and the recipe only bbwarns about it.
# Shipping that means a device with no usable keyring, which does not fail at
# build time and does not fail at boot: it fails the first time someone tries
# to update a device in the field.
#
# med-keyring.pem holds the MedPlatform root CA *and* a CRL in one PEM file
# (OpenSSL's X509_load_cert_crl_file reads both), because system.conf sets
# check-crl=true and OpenSSL then demands a CRL for every certificate in the
# chain. Regenerate both with scripts/med-pki.sh - the private keys it produces
# stay outside this repository.
RAUC_KEYRING_FILE = "med-keyring.pem"

# The bootloader is a property of the machine, not of the distro. qemux86-64
# boots through GRUB/EFI (see med-partitions.wks) and the STM32MP257 through
# U-Boot; naming either one in this layer would make the OS policy layer carry
# BSP knowledge, which is the coupling MED_EEG_DRIVER exists to avoid for the
# acquisition front-end. Same treatment, same reason.
#
# The default is "noop" rather than a real backend: a target that forgets to
# declare its bootloader gets a RAUC that starts, enumerates its slots and
# declines to mark anything bootable - which is a legible failure, unlike a
# dead service or, worse, an A/B switch the bootloader will not honour.
MED_BOOTLOADER ?= "noop"

# system.conf is a plain configuration file, so bitbake does not expand
# variables inside it. The compatible string has to match the machine the
# bundle was built for, otherwise RAUC refuses to install - substitute it here
# instead of hardcoding a machine name in the layer.
do_install:append() {
    if [ -f ${D}${sysconfdir}/rauc/system.conf ]; then
        sed -i -e "s|@MED_COMPATIBLE@|${DISTRO}-${MACHINE}|g" \
               -e "s|@MED_VERSION@|${DISTRO_VERSION}|g" \
               -e "s|@MED_BOOTLOADER@|${MED_BOOTLOADER}|g" \
               ${D}${sysconfdir}/rauc/system.conf
    fi

    # A leftover placeholder means the substitution above silently missed one,
    # and the failure would only surface on a device. Fail the build instead.
    if grep -q "@MED_[A-Z_]*@" ${D}${sysconfdir}/rauc/system.conf; then
        bbfatal "unsubstituted placeholder left in system.conf: \
$(grep -o '@MED_[A-Z_]*@' ${D}${sysconfdir}/rauc/system.conf | sort -u | tr '\n' ' ')"
    fi
}
