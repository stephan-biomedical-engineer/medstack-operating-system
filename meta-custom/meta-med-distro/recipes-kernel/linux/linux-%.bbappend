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

# --- O fragmento de bancada, e o guarda que o mantém fora do produto.
#
# MED_BENCH = "1" acrescenta símbolos que existem para medir a placa e que não
# pertencem a um aparelho entregue - hoje CONFIG_SPI_SPIDEV, que a Fase 2 do
# plano de bancada exige e que é um caminho de userspace para o barramento do
# conversor.
#
# Declarado com "?=" e vazio: um perfil que não pede não ganha. E a recusa
# abaixo é o que impede o erro que realmente acontece, que não é alguém ligar o
# bench de propósito - é alguém esquecer de desligar.
MED_BENCH ?= ""

python () {
    if not bb.data.inherits_class('kernel', d):
        return
    if (d.getVar('MED_BENCH') or '').strip() != '1':
        return

    # A bancada que estes símbolos servem é a da ponte USB, e só ela. O spidev
    # existe para a Fase 2 do plano de bancada, que mede a máquina de estados
    # de transferência do MCP2210 com um jumper - e o MCP2210 só está no
    # caminho quando MED_EEG_LINK = "usb".
    #
    # Sem esta recusa a combinação silenciosa é amp + spidev: um perfil de
    # produto cujo conversor está no co-processador, carregando um nó de
    # userspace para um barramento que ele nem usa. Foi exatamente o que um
    # build de verificação produziu em 2026-09-28, e o que o convidou foi o
    # MED_BENCH ser ortogonal ao link.
    #
    # E a recusa é útil na outra direção também: MED_BENCH sem "usb" não
    # produz kernel-module-hid-mcp2210 nem CONFIG_HID_MCP2210, então seria uma
    # imagem de bancada sem a ponte que a bancada mede.
    link = (d.getVar('MED_EEG_LINK') or '').strip()
    if link != 'usb':
        bb.fatal(
            "MED_BENCH = 1 with MED_EEG_LINK = '%s'.\n"
            "The bench kernel symbols serve the USB bridge, and the bridge is "
            "only in the path on the 'usb' link. On any other link this builds "
            "a spidev node for a bus the profile does not use, and leaves the "
            "bench without the driver it came to measure." % (link or '(unset)'))

    d.appendVar('SRC_URI', ' file://med-kernel-bench.cfg')
    d.appendVar('KERNEL_CONFIG_FRAGMENTS', ' ${WORKDIR}/med-kernel-bench.cfg')
    d.appendVar('MED_KERNEL_REQUIRED_CFG', ' ${WORKDIR}/med-kernel-bench.cfg')

    d.appendVarFlag('do_configure', 'prefuncs', ' med_announce_bench')
}

# O aviso é uma TAREFA e não uma linha no python anônimo, e a diferença é
# medida: como anônimo ele saía ~30 vezes num único build - uma por parse da
# receita, e o bitbake reparseia nos workers - de um total de 42 avisos. Um
# aviso repetido trinta vezes não é ênfase, é ruído, e ruído treina a pessoa a
# ignorar avisos. Aqui ele sai uma vez, quando o kernel é configurado.
#
# E ele diz menos do que dizia, de propósito. Antes carregava o peso de ser a
# única barreira ("must not be shipped"); agora as duas recusas duras carregam
# isso - uma imagem endurecida com MED_BENCH falha em do_rootfs, e MED_BENCH
# fora do link usb falha no parse. O que sobra para o aviso é o caso
# PERMITIDO: uma imagem de desenvolvimento em que a pessoa deve saber o que o
# kernel ganhou.
python med_announce_bench() {
    bb.warn("MED_BENCH = 1: this kernel has bench-only symbols "
            "(med-kernel-bench.cfg, today CONFIG_SPI_SPIDEV). Allowed here "
            "because the image is a development one; a hardened image with "
            "MED_BENCH is refused at do_rootfs.")
}


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
