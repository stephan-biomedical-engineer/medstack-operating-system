SUMMARY = "MedPlatform U-Boot environment image: the A/B bootcmd, written into u-boot-env by wic"
DESCRIPTION = "Builds the binary U-Boot environment that the u-boot-env partition \
carries: the vendor's own default environment with MedPlatform's A/B slot \
selection applied on top. Shipping it in the image is what makes a freshly \
flashed card boot with BOOT_ORDER already set, instead of needing fw_setenv by \
hand. See docs/implementation_plan_uboot_ab.md."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://med-bootcmd.env"
S = "${WORKDIR}"
B = "${WORKDIR}/build"

# The content is a bootcmd carrying PARTUUIDs from one machine's partition
# table. Two machines sharing a TMPDIR would otherwise write the same file name
# into the same deploy directory with different contents, and the second build
# would silently overwrite the first - the hazard med-uboot-env-config and
# med-data-volume both record.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# Data only: nothing is compiled and nothing is packaged. The artefact is a
# deploy-time input to wic, not a package in the rootfs.
#
# "inherit nopackages", not PACKAGES = "". The empty list looks equivalent and
# is not: do_package still runs, sees nothing to do, and skips before creating
# packages-split/ - and buildhistory's do_package postfunc then fails on
# "find .../packages-split/*: No such file or directory". nopackages deletes the
# packaging tasks outright, so nothing is left to hang a postfunc on.
INHIBIT_DEFAULT_DEPS = "1"
inherit nopackages

DEPENDS = "u-boot-mkenvimage-native"

# The vendor's default environment, which meta-st deploys itself. This is the
# only reason the task ordering exists - see §4.3.1 of the plan for why a
# partial environment is not an option.
do_compile[depends] += "u-boot-stm32mp:do_deploy"

inherit deploy

# All five come from conf/layer.conf, per machine. Empty means "this machine has
# no MedPlatform boot environment", which is the normal state of qemux86-64.
MED_ROOT_A_PARTUUID ?= ""
MED_ROOT_B_PARTUUID ?= ""
MED_UBOOT_ENV_SIZE ?= ""
MED_UBOOT_ENV_PART_SIZE_K ?= ""
MED_UBOOT_INITIAL_ENV ?= ""

# Declared in meta-med-distro/conf/distro/med-os.conf, because the number of
# boot attempts is update policy rather than a board fact. Reading a variable a
# higher layer owns is the same soft coupling as MED_WKS_FILE in the other
# direction, and is as far as the coupling goes - no bbappend, no include.
MED_BOOT_ATTEMPTS ?= "3"

python () {
    if not d.getVar('MED_ROOT_A_PARTUUID'):
        raise bb.parse.SkipRecipe(
            "no MedPlatform U-Boot environment is defined for MACHINE=%s; "
            "declare MED_ROOT_A_PARTUUID in meta-med-bsp/conf/layer.conf if "
            "this board needs one" % d.getVar('MACHINE'))
}

# Variables that the vendor default environment must provide, because the
# bootcmd or its rescue path dereferences them. Absent means the board would
# boot - through the rescue path - and nothing would look wrong, which is the
# reason this is a build failure rather than a runtime discovery.
# All seven are present in meta-st's u-boot-initial-env for stm32mp25 - checked,
# not assumed - so any absence means the vendor changed something.
MED_UBOOT_ENV_REQUIRED = "bootcmd kernel_addr_r fdt_addr_r kernel_comp_addr_r kernel_comp_size console baudrate"

# Wanted but not fatal: ST's board code sets these at runtime rather than
# compiling them into the default environment, so their absence here is the
# expected state and only worth reporting. med_seed has fallbacks for the first
# two; without fdtfile, med_boot fails its load and bootcmd takes the rescue
# path, which is the designed degradation.
MED_UBOOT_ENV_EXPECTED = "boot_device boot_instance fdtfile"

def med_env_read_fragment(path, subst):
    """Read med-bootcmd.env: drop comments and blank lines, join continuations,
    substitute the @MED_*@ placeholders. Returns an ordered list of (key, value).
    """
    out = []
    pending = ""
    with open(path, "r") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not pending and (not line.strip() or line.lstrip().startswith("#")):
                continue
            if line.endswith("\\"):
                pending += line[:-1].strip() + " "
                continue
            line = (pending + line.strip()).strip()
            pending = ""
            if "=" not in line:
                bb.fatal("med-bootcmd.env: line without '=': %s" % line)
            key, value = line.split("=", 1)
            for placeholder, replacement in subst.items():
                value = value.replace(placeholder, replacement)
            leftover = [w for w in value.split() if w.startswith("@MED_") and w.endswith("@")]
            if leftover:
                bb.fatal("med-bootcmd.env: unsubstituted placeholder(s) %s in %s" %
                         (" ".join(leftover), key))
            out.append((key.strip(), value.strip()))
    if pending:
        bb.fatal("med-bootcmd.env: file ends with a continuation backslash")
    return out

def med_env_parse_blob(blob, env_size, redundant=True):
    """Split one environment copy into (stored_crc, computed_crc, {k: v})."""
    import struct, zlib
    header = 5 if redundant else 4
    stored = struct.unpack("<I", blob[0:4])[0]
    data = blob[header:env_size]
    computed = zlib.crc32(data) & 0xffffffff
    entries = {}
    for item in data.split(b"\0"):
        if not item:
            break
        text = item.decode("utf-8", "replace")
        if "=" in text:
            k, v = text.split("=", 1)
            entries[k] = v
    return stored, computed, entries

python do_compile() {
    import os, shutil

    deploydir = d.getVar('DEPLOY_DIR_IMAGE')
    env_size  = int(d.getVar('MED_UBOOT_ENV_SIZE'), 16)
    part_size = int(d.getVar('MED_UBOOT_ENV_PART_SIZE_K')) * 1024
    workdir   = d.getVar('B')

    if part_size < 2 * env_size:
        bb.fatal("u-boot-env partition (%d bytes) cannot hold two %d-byte "
                 "environment copies" % (part_size, env_size))

    # ------------------------------------------------------------------
    # Guard: slot A's PARTUUID is not ours to choose.
    #
    # meta-st declares DEVICE_PARTUUID_ROOTFS:mmc0 and generates the vendor
    # extlinux.conf with root=PARTUUID= from it. That file is the rescue path,
    # so if the disk stops matching it the rescue path silently stops working -
    # and by definition nobody exercises a rescue path until they need it.
    # Until now the two agreed because the .wks repeated the literal.
    # ------------------------------------------------------------------
    root_a = d.getVar('MED_ROOT_A_PARTUUID')
    vendor_a = d.getVar('DEVICE_PARTUUID_ROOTFS:SDCARD')
    if vendor_a and vendor_a != root_a:
        bb.fatal("MED_ROOT_A_PARTUUID (%s) does not match the vendor's "
                 "DEVICE_PARTUUID_ROOTFS:SDCARD (%s). The BSP generates "
                 "extlinux.conf with root=PARTUUID=%s, and that file is the "
                 "rescue path for a lost boot environment; a mismatch means the "
                 "rescue path names a partition that does not exist."
                 % (root_a, vendor_a, vendor_a))

    # ------------------------------------------------------------------
    # Start from U-Boot's own default environment. Not optional: a stored
    # environment replaces the built-in one entirely (env_import -> himport_r
    # without H_NOCLEAR), it does not merge.
    # ------------------------------------------------------------------
    initial = d.getVar('MED_UBOOT_INITIAL_ENV')
    if not os.path.exists(initial):
        # Search the whole deploy tree, not just its top level: meta-st sets
        # do_deploy[sstate-outputdirs] to a subdirectory, and a diagnostic that
        # looks in the wrong place reports absence instead of pointing at the
        # file - which is worse than no diagnostic, because it is believed.
        found = []
        for root, _dirs, files in os.walk(deploydir):
            for name in files:
                if name.startswith('u-boot-initial-env'):
                    found.append(os.path.relpath(os.path.join(root, name), deploydir))
        bb.fatal("MED_UBOOT_INITIAL_ENV points at %s, which does not exist. "
                 "Under %s there is: %s. Set the variable in "
                 "meta-med-bsp/conf/layer.conf to the one whose U-Boot ends up "
                 "in the FIP this board boots."
                 % (initial, deploydir, ", ".join(sorted(found)) or "no u-boot-initial-env at all"))

    merged = []
    seen = {}
    with open(initial, "r") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            seen[k] = len(merged)
            merged.append((k, v))

    # Checked against the *vendor's* keys, before ours are merged in. Running
    # this after the merge would be worse than useless: we add bootcmd
    # ourselves, so a vendor environment that had lost it would still pass.
    vendor_keys = set(k for k, _ in merged)
    missing = [k for k in (d.getVar('MED_UBOOT_ENV_REQUIRED') or "").split()
               if k not in vendor_keys]
    if missing:
        bb.fatal("%s does not define %s. The MedPlatform bootcmd and its "
                 "rescue path dereference these; without them the board would "
                 "boot only through the vendor path, which looks exactly like "
                 "success." % (os.path.basename(initial), ", ".join(missing)))
    absent = [k for k in (d.getVar('MED_UBOOT_ENV_EXPECTED') or "").split()
              if k not in vendor_keys]
    if absent:
        bb.note("not compiled into the U-Boot default environment: %s. Expected "
                "- ST's board code sets these at runtime. If med_boot fails on "
                "the target, they are the first things to print at the U-Boot "
                "prompt." % ", ".join(absent))

    attempts = d.getVar('MED_BOOT_ATTEMPTS')
    if not (attempts.isdigit() and 1 <= int(attempts) <= 9):
        bb.fatal("MED_BOOT_ATTEMPTS is %r; it must be 1..9. RAUC writes the "
                 "counter with \"%%x\" and U-Boot's setexpr reads hexadecimal "
                 "while its test reads decimal - the two agree only below 10, "
                 "and above it they disagree silently." % attempts)

    # The rescue path, captured rather than written down. bootcmd is about to be
    # replaced, so whatever it was becomes med_rescue - which makes the fallback
    # *by construction* exactly what this board did before any of this existed,
    # instead of a guess that has to be kept in step with the vendor. On
    # stm32mp25 it is "run bootcmd_stm32mp"; hardcoding that would have been
    # right today and silently wrong after a BSP bump.
    rescue = dict(merged)['bootcmd']

    ours = med_env_read_fragment(
        os.path.join(d.getVar('S'), 'med-bootcmd.env'),
        {'@MED_ROOT_A_PARTUUID@': root_a,
         '@MED_ROOT_B_PARTUUID@': d.getVar('MED_ROOT_B_PARTUUID'),
         '@MED_BOOT_ATTEMPTS@': attempts,
         '@MED_VENDOR_BOOTCMD@': rescue})

    overridden = []
    for k, v in ours:
        if k in seen:
            overridden.append(k)
            merged[seen[k]] = (k, v)
        else:
            seen[k] = len(merged)
            merged.append((k, v))
    bb.note("med-uboot-env-image: %d variables from %s, %d from med-bootcmd.env "
            "(%d overriding: %s)" % (len(merged) - len(ours) + len(overridden),
                                     os.path.basename(initial), len(ours),
                                     len(overridden), " ".join(overridden) or "-"))

    bb.utils.mkdirhier(workdir)
    txt = os.path.join(workdir, 'med-uboot-env.txt')
    with open(txt, "w") as f:
        for k, v in merged:
            f.write("%s=%s\n" % (k, v))

    # ------------------------------------------------------------------
    # One environment copy, in U-Boot's own format. mkenvimage rather than
    # hand-rolled bytes: the layout (crc32, then a flags byte because the
    # environment is redundant, then NUL-separated key=value) belongs to U-Boot
    # and is not ours to reimplement. Placement below is ours, because it comes
    # from the .wks.
    # ------------------------------------------------------------------
    # mkenvimage refuses an oversized environment, but its message names bytes
    # and not the cause. The vendor default is most of the payload.
    payload = sum(len(k) + len(v) + 2 for k, v in merged) + 1
    if payload > env_size - 5:
        bb.fatal("the merged environment needs %d bytes and CONFIG_ENV_SIZE is "
                 "%d (minus 4 CRC bytes and 1 redundancy flag byte). %d of the "
                 "%d variables come from %s."
                 % (payload, env_size, len(merged) - len(ours),
                    len(merged), os.path.basename(initial)))

    copy = os.path.join(workdir, 'med-uboot-env-copy.bin')
    bb.process.run("%s/mkenvimage -r -s %d -o %s %s"
                   % (d.getVar('STAGING_BINDIR_NATIVE'), env_size, copy, txt))

    with open(copy, "rb") as f:
        blob = f.read()
    if len(blob) != env_size:
        bb.fatal("mkenvimage produced %d bytes, expected %d" % (len(blob), env_size))

    # ------------------------------------------------------------------
    # Placement. U-Boot's mmc_offset_try_partition() puts copy 0 in the last
    # CONFIG_ENV_SIZE bytes of the partition and copy 1 in the CONFIG_ENV_SIZE
    # bytes before it - the same formula med-uboot-env-config derives the
    # fw_env.config offsets (-0x2000, -0x4000) from. One derivation, two
    # consumers; if they ever disagree, fw_printenv reads a different place than
    # U-Boot writes and neither reports anything.
    #
    # Both copies are written identical. env_import_redund() prefers the higher
    # flags byte and falls back to the first copy when they are equal, so equal
    # is well defined.
    # ------------------------------------------------------------------
    image = bytearray(b"\x00" * part_size)
    image[part_size - env_size:part_size] = blob
    image[part_size - 2 * env_size:part_size - env_size] = blob

    out = os.path.join(workdir, 'med-uboot-env.bin')
    with open(out, "wb") as f:
        f.write(image)
    bb.note("med-uboot-env-image: %d-byte image, copies at 0x%x and 0x%x"
            % (part_size, part_size - 2 * env_size, part_size - env_size))
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 ${B}/med-uboot-env.bin ${DEPLOYDIR}/med-uboot-env.bin
    install -m 0644 ${B}/med-uboot-env.txt ${DEPLOYDIR}/med-uboot-env.txt
}

# ------------------------------------------------------------------
# Verify what actually ships, not what was intended.
#
# Every failure this checks for produces the same symptom on the board: U-Boot
# discards the environment, falls back to its built-in default, boots slot A
# through the vendor extlinux.conf, and everything looks normal. A/B selection
# would simply never happen and nothing would say so. That is the reason these
# are assertions in the build rather than items on a bench checklist.
# ------------------------------------------------------------------
python med_verify_env_image() {
    import os

    out = os.path.join(d.getVar('DEPLOYDIR'), 'med-uboot-env.bin')
    env_size  = int(d.getVar('MED_UBOOT_ENV_SIZE'), 16)
    part_size = int(d.getVar('MED_UBOOT_ENV_PART_SIZE_K')) * 1024

    with open(out, "rb") as f:
        image = f.read()

    if len(image) != part_size:
        bb.fatal("%s is %d bytes; the u-boot-env partition is %d. wic rawcopy "
                 "writes from the start of the partition, so a short image puts "
                 "both copies at the wrong offsets." % (out, len(image), part_size))

    copy0 = image[part_size - env_size:part_size]
    copy1 = image[part_size - 2 * env_size:part_size - env_size]
    if copy0 != copy1:
        bb.fatal("the two environment copies in %s differ" % out)

    stored, computed, entries = med_env_parse_blob(copy0, env_size)
    if stored != computed:
        bb.fatal("environment CRC mismatch in %s: stored 0x%08x, computed "
                 "0x%08x. The CRC covers the data area only - it excludes the "
                 "4 CRC bytes *and* the redundancy flags byte." % (out, stored, computed))

    required = ['bootcmd', 'BOOT_ORDER', 'BOOT_A_LEFT', 'BOOT_B_LEFT',
                'med_root_a', 'med_root_b', 'med_rescue']
    missing = [k for k in required if k not in entries]
    if missing:
        bb.fatal("%s does not define %s" % (out, ", ".join(missing)))

    # The factory state, which is the whole reason this image is built rather
    # than left to a first-boot script: a card must come out of bmaptool with
    # these already set. Before 2026-08-30 nothing wrote them at any point in a
    # device's life, so "rauc status" on a new board reported every slot bad.
    if entries.get('BOOT_ORDER') != 'A B':
        bb.fatal("BOOT_ORDER is %r, expected 'A B'" % entries.get('BOOT_ORDER'))
    if entries.get('BOOT_A_LEFT') != d.getVar('MED_BOOT_ATTEMPTS'):
        bb.fatal("BOOT_A_LEFT is %r, expected %r"
                 % (entries.get('BOOT_A_LEFT'), d.getVar('MED_BOOT_ATTEMPTS')))
    if entries.get('BOOT_B_LEFT') != '0':
        bb.fatal("BOOT_B_LEFT is %r, expected '0'. Slot B on a freshly flashed "
                 "card has never been written; a non-zero counter there asserts "
                 "that a blank partition is bootable."
                 % entries.get('BOOT_B_LEFT'))
    if entries.get('med_root_a') != d.getVar('MED_ROOT_A_PARTUUID'):
        bb.fatal("med_root_a is %r, expected %r"
                 % (entries.get('med_root_a'), d.getVar('MED_ROOT_A_PARTUUID')))
    if entries.get('med_root_b') != d.getVar('MED_ROOT_B_PARTUUID'):
        bb.fatal("med_root_b is %r, expected %r"
                 % (entries.get('med_root_b'), d.getVar('MED_ROOT_B_PARTUUID')))
    if 'rauc.slot=' not in entries.get('med_bootargs', ''):
        bb.fatal("med_bootargs does not pass rauc.slot=. Without it "
                 "rauc-mark-good.service stays skipped by "
                 "ConditionKernelCommandLine, the attempt counter is never "
                 "restored, and the device changes slot after three successful "
                 "boots.")

    # A partial environment is the failure mode §4.3.1 of the plan exists to
    # prevent, and it has a shape: a handful of variables instead of dozens.
    # The board measured 62 on 2026-08-30.
    if len(entries) < 20:
        bb.fatal("%s holds only %d variables. A stored environment replaces "
                 "U-Boot's built-in one rather than merging with it, so this "
                 "would produce a board with no distro_bootcmd and no rescue "
                 "path." % (out, len(entries)))

    bb.note("med-uboot-env-image: verified %d variables, CRC 0x%08x, two copies"
            % (len(entries), stored))
}
do_deploy[postfuncs] += "med_verify_env_image"

addtask deploy after do_compile before do_build
