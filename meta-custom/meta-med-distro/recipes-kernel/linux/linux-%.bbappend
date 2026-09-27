# MedOS kernel policy, applied to whatever kernel the BSP happens to provide.
#
# The wildcard is deliberate (see CLAUDE.md rule 3): linux-yocto on QEMU,
# linux-stm32mp on the STM32MP257 and linux-raspberrypi on a Pi must all end up
# with the same distro level kernel capabilities, and the app/framework layers
# must never have to know which one is in use. Porting to a new BSP therefore
# costs zero lines here, which is exactly what the portability metric measures.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# "linux-%" also matches linux-libc-headers, linux-firmware and friends, which
# are not kernels and would fail on an unexpected SRC_URI entry. Only extend
# recipes that actually inherit the kernel class.
#
# Two mechanisms, because "kernel recipe" is not one thing:
#
#   kernel-yocto (linux-yocto) scans SRC_URI for .cfg files by itself and feeds
#   them to its own configuration machinery. SRC_URI alone is enough there, and
#   that is why this bbappend worked on QEMU and looked portable.
#
#   plain kernel.bbclass (linux-stm32mp, and most vendor kernels) does not. ST's
#   linux-stm32mp.inc merges exactly the files listed in KERNEL_CONFIG_FRAGMENTS
#   with merge_config.sh and ignores everything else in SRC_URI. Appending to
#   SRC_URI alone therefore *unpacked* this fragment into the work directory and
#   never applied a line of it.
#
# That was not a hypothesis: KERNEL_CONFIG_FRAGMENTS for linux-stm32mp listed
# ST's four fragments and not this one, while SRC_URI listed this one - so every
# symbol MedOS asks for was, on that target, whatever ST's defconfig happened to
# say. The ones that came out right came out right by coincidence, which is the
# worst way for a security requirement to be satisfied. Appending last matters:
# merge_config.sh applies fragments in order and the last assignment wins.
# ---------------------------------------------------------------------------
# Did the fragment actually take?
# ---------------------------------------------------------------------------
# Declaring a symbol and getting it are different things, and this repository has
# paid for the difference three times: CONFIG_DM_VERITY was missing while
# linux-yocto's defconfig hid it and only the board would have failed; CONFIG_SPI
# and CONFIG_GPIOLIB were missing with the roles reversed. Each time the build
# was green.
#
# The out-of-tree module recipes used to catch part of this, because a module
# fails to build against a kernel that lacks what it needs. With the AFE driver
# now in the kernel tree, that accidental check is gone: a CONFIG_TI_ADS1299 that
# never took produces an image with no front-end driver and no error anywhere.
#
# So the check becomes explicit, and it is about the FINAL .config - after
# merge_config.sh, after the defconfig, after every other fragment. A symbol our
# fragment asks for and does not get is a build failure naming it.
#
# Deliberately not a warning. A warning about a security capability that was
# silently dropped is a warning nobody reads on the build that ships.
# What the guard validates is not "every fragment in the build" - that would mix
# our requirements with the vendor's and make us the owner of ST's defconfig.
# It validates exactly the fragments MedOS declares as its own, and each layer
# that has requirements appends its fragment to this list. Today that is two:
# the distro's capabilities, always, and the drivers of one kernel tree, only
# when that tree is what is being built.
MED_KERNEL_REQUIRED_CFG ?= ""

python do_med_check_kernel_config() {
    import os
    import re

    fragments = (d.getVar('MED_KERNEL_REQUIRED_CFG') or '').split()
    if not fragments:
        bb.fatal("MED_KERNEL_REQUIRED_CFG is empty: this bbappend claims MedOS "
                 "has kernel requirements and then declares none")

    final = os.path.join(d.getVar('B'), '.config')
    if not os.path.exists(final):
        bb.fatal("no kernel .config at %s - cannot verify what MedOS asked for"
                 % final)

    # Which fragment asked for what, so a failure names the file a reader has to
    # open and not just the symbol.
    want_on = {}
    want_off = {}
    for frag in fragments:
        if not os.path.exists(frag):
            bb.fatal("%s is declared in MED_KERNEL_REQUIRED_CFG and does not "
                     "exist. A requirement that cannot be read is not a "
                     "requirement." % frag)
        name = os.path.basename(frag)
        for line in open(frag):
            line = line.strip()
            m = re.match(r'^# (CONFIG_[A-Z0-9_]+) is not set$', line)
            if m:
                want_off[m.group(1)] = name
                continue
            if not line.startswith('CONFIG_') or '=' not in line:
                continue
            key, value = line.split('=', 1)
            want_on[key] = (value, name)

    have = {}
    for line in open(final):
        line = line.strip()
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1)
            have[key] = value

    # Only tristates are checked. A string or a number ("CONFIG_X=\"foo\"", =128)
    # can legitimately be overridden by a later fragment with a different value,
    # and this check is about capabilities being present, not about who won.
    problems = []
    for sym in sorted(want_on):
        value, frag = want_on[sym]
        if value in ('y', 'm') and have.get(sym) not in ('y', 'm'):
            problems.append("%s: asked for by %s, not in the final .config"
                            % (sym, frag))
    for sym in sorted(want_off):
        if have.get(sym) in ('y', 'm'):
            problems.append("%s: asked to be off by %s, present as %s"
                            % (sym, want_off[sym], have[sym]))

    if problems:
        bb.fatal(
            "the kernel configured for MACHINE=%s does not provide what MedOS "
            "declares.\n  %s\n"
            "Two causes worth separating before treating this as a gap. A symbol "
            "with no Kconfig prompt cannot be turned on by a fragment at all - "
            "CONFIG_REGMAP is one, and it is selected by drivers. And a symbol "
            "that does not EXIST in this kernel's Kconfig is dropped in silence: "
            "that is content of a particular tree rather than a capability, and "
            "it does not belong in a fragment applied to every kernel."
            % (d.getVar('MACHINE'), '\n  '.join(problems)))

    bb.note("med: %d kernel symbols declared across %d fragment(s), all satisfied"
            % (len(want_on) + len(want_off), len(fragments)))
}

python __anonymous() {
    if not bb.data.inherits_class('kernel', d):
        return
    d.appendVar('SRC_URI', ' file://med-kernel-features.cfg')
    d.appendVar('KERNEL_CONFIG_FRAGMENTS', ' ${WORKDIR}/med-kernel-features.cfg')
    d.appendVar('MED_KERNEL_REQUIRED_CFG', ' ${WORKDIR}/med-kernel-features.cfg')
    bb.build.addtask('do_med_check_kernel_config', 'do_compile', 'do_configure', d)
}
