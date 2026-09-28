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
# EXERCISED. A full image build on 2026-09-27 produced
# 6.6.129-stm32mp-r3.1+med at revision da723f985714, and a kernel build the same
# day at 013e9b1ab0f2 compiled both front-end drivers as modules with the
# symbols present in the final .config. What that first build also showed is
# that the containerised path dropped MED_KERNEL_GIT entirely - kas-container
# forwards a fixed whitelist - so this file's own documented invocation only
# worked with NATIVE=1 until the Makefile gained KERNEL_ARGS.

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
    #
    # A marker and not the revision, which this used to append as well. The
    # revision was already in the package version from elsewhere and in the
    # module path (usr/lib/modules/6.6.129-g013e9b1ab0f2), so carrying a third
    # copy bought no traceability - and it bought a defect, because a hash has
    # no order. Moving the branch forward from da723f985714 to 013e9b1ab0f2
    # made do_packagedata refuse the build with version-going-backwards, and it
    # would have done so on every commit.
    d.appendVar('PV', '+med')

    # And the half of that defect this file cannot remove.
    #
    # The revision still reaches the package version through a component this
    # append does not control, rendered with AUTOINC at 0 because there is no
    # PR service - so the ordering still comes down to comparing two hashes and
    # version-going-backwards still fires.
    #
    # It is downgraded to a warning, for this recipe and only when building
    # from the fork, because the scenario it protects does not exist here. That
    # check guards a PACKAGE FEED: an incremental client that would refuse an
    # upgrade whose version went down. This project has no feed. It builds
    # whole images, and it updates them through RAUC A/B bundles that replace
    # the entire rootfs - implementation_plan_rauc.md. There is no client
    # comparing package versions anywhere in the path.
    #
    # The alternative was a PR service (PRSERV_HOST), which is the mechanism
    # designed for exactly this and makes the ordering true rather than
    # excused. It is refused because it keeps its counter in a machine-local
    # sqlite database, and a build whose version depends on how many times THIS
    # host has built is a worse trade for a project whose reproducibility is a
    # claim it makes.
    d.appendVar('WARN_QA', ' version-going-backwards')
    d.setVar('ERROR_QA', ' '.join(
        sym for sym in (d.getVar('ERROR_QA') or '').split()
        if sym != 'version-going-backwards'))

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

    # The bridge is loaded at boot, and that is not a convenience.
    #
    # hid_generic_match() declines any device that some other registered HID
    # driver matches (drivers/hid/hid-generic.c:37-57), so winning the bind
    # needs no quirk - but "registered" is the operative word. If the bridge
    # enumerates before this module is loaded, hid-generic, which is built in
    # and always present, binds it first, and nothing hands it over afterwards
    # without a manual unbind. Autoloading removes the race instead of
    # documenting it.
    #
    # This travelled here from the recipe that used to build the module
    # out-of-tree, deleted on 2026-09-27. A module changing where it is built
    # must not change whether it loads.
    if 'CONFIG_HID_MCP2210' in symbols:
        d.appendVar('KERNEL_MODULE_AUTOLOAD', ' hid-mcp2210')
}
