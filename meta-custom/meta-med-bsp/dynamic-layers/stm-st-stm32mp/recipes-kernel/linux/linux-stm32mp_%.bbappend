# Where this board's kernel comes from.
#
# The two front-end drivers live in a Linux tree that is their single source of
# truth, and this repository integrates and builds rather than holding their
# code - implementation_plan_ads1299_upstream.md §2.2. So the Yocto build has to
# be able to consume a specific revision of that tree.
#
# This is a BUILD PARAMETER with a null default, in the shape
# MED_DATA_KEY_SOURCE already uses: unset, the build is exactly ST's, tarball
# 6.6.129 plus the r3.1 patch, byte for byte what has already been measured. Set,
# the same base comes from git instead - the equivalence of the two was proved
# before this file existed, with scripts/med-kernel-fingerprint.sh (81889 files,
# 0 divergent hashes, digest 35e19311...).
#
#   MED_KERNEL_GIT="git:///path/to/linux-med;protocol=file;branch=<branch>" \
#   MED_KERNEL_SRCREV=<sha> make stm32
#
# Why not class-devupstream, which ST already ships for exactly this: selecting
# that variant needs a PREFERRED_VERSION carrying ${SRCPV}, and the variant as
# published never overrides S, so it points at ${WORKDIR}/linux-6.6.129 while a
# git fetch unpacks into ${WORKDIR}/git. Overriding the normal recipe avoids both
# problems, and it makes the pending question about S:class-devupstream moot
# rather than answered.
#
# NOT YET EXERCISED IN A BUILD. Everything below is reasoning about ST's recipe,
# and the first `make stm32` with these variables set is what turns it into fact.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

MED_KERNEL_GIT ??= ""
MED_KERNEL_SRCREV ??= ""

python () {
    git = (d.getVar('MED_KERNEL_GIT') or '').strip()
    if not git:
        return

    rev = (d.getVar('MED_KERNEL_SRCREV') or '').strip()
    if not rev:
        bb.fatal("MED_KERNEL_GIT is set and MED_KERNEL_SRCREV is not. A floating "
                 "branch tip is not a kernel a measurement can cite: pin the "
                 "commit, or unset both and build ST's release.")

    # Keep ONLY the local files, and of those not the patches. What has to
    # survive is ST's kernel config fragments, which the recipe lists in
    # KERNEL_CONFIG_FRAGMENTS and without which the build is unusable. What has
    # to go is ST's tarball, ST's r3.1 patch (already in the fork's history, and
    # applying it twice would fail) and any other SCM.
    #
    # An allow-list and not a deny-list, because the first version of this was a
    # deny-list and got it wrong: it dropped http:// and https:// and let
    # "git://github.com/STMicroelectronics/linux.git;protocol=https" through,
    # since that entry starts with git:// and not https://. The result was two
    # SCMs in SRC_URI and a parse failure - in the class-devupstream variant,
    # which BBCLASSEXTEND makes bitbake parse whether we select it or not.
    keep = [entry for entry in (d.getVar('SRC_URI') or '').split()
            if entry.startswith('file://') and '.patch' not in entry]

    d.setVar('SRC_URI', ' '.join([git] + keep))
    d.setVar('SRCREV', rev)
    d.setVar('S', '${WORKDIR}/git')

    # So that a build made from the fork is never mistaken for ST's release in a
    # manifest, an SBOM or a measurement.
    d.appendVar('PV', '+med%s' % rev[:12])

    # The driver fragment, and it takes TWO conditions rather than one.
    #
    # The first is the branch we are already in: the tree being built contains
    # our drivers, so their Kconfig symbols exist. Asking for a symbol a kernel
    # does not have is a request merge_config.sh drops in silence, which is how
    # CONFIG_TI_ADS1299 spent one build in the distro fragment before the guard
    # caught it.
    #
    # The second is that the active link actually uses the driver, and that
    # question is answered by meta-med-afe-ads1299 and read here. Enabling the
    # symbol for a link that does not need it does not merely waste build time:
    # the image installs the kernel-modules metapackage, which depends on every
    # module the kernel builds, so an enabled symbol IS a module in the product
    # image. That happened - the converter driver shipped in the "amp" profile,
    # whose entire argument is that the Linux side carries no converter code -
    # and no build said a word about it.
    symbols = (d.getVar('MED_AFE_KCONFIG') or '').split()
    if not symbols:
        return

    # The variable names the symbols and the fragment carries them. Two places,
    # so they are checked against each other here rather than trusted: a link
    # that declares a symbol the fragment does not set would be a request that
    # silently never happens.
    import os
    frag = None
    for path in (d.getVar('FILESPATH') or '').split(':'):
        candidate = os.path.join(path, 'med-stm32mp-drivers.cfg')
        if os.path.exists(candidate):
            frag = candidate
            break
    if not frag:
        bb.fatal("med-stm32mp-drivers.cfg is not on FILESPATH, and "
                 "MED_AFE_KCONFIG asks for %s" % ' '.join(symbols))
    text = open(frag).read()
    absent = [sym for sym in symbols
              if ('%s=y' % sym) not in text and ('%s=m' % sym) not in text]
    if absent:
        bb.fatal("MED_AFE_KCONFIG declares %s for MED_EEG_LINK = '%s', and "
                 "med-stm32mp-drivers.cfg does not set them. The declaration and "
                 "the fragment have to agree, or the symbol is asked for by "
                 "nobody." % (' '.join(absent), d.getVar('MED_EEG_LINK')))

    # Appended to MED_KERNEL_REQUIRED_CFG as well as to the two variables that
    # apply it: a fragment that is applied and not verified is the silent half
    # of the same defect.
    d.appendVar('SRC_URI', ' file://med-stm32mp-drivers.cfg')
    d.appendVar('KERNEL_CONFIG_FRAGMENTS', ' ${WORKDIR}/med-stm32mp-drivers.cfg')
    d.appendVar('MED_KERNEL_REQUIRED_CFG', ' ${WORKDIR}/med-stm32mp-drivers.cfg')
}
