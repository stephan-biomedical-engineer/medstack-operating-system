#!/usr/bin/env python3
"""Fault injection for scripts/med-layer-check.py (docs/implementation_plan_layer_check.md §5.1).

A check that has never been seen failing is a claim. Each case below copies
meta-custom to a temporary directory, injects one violation, runs the check
over the copy and compares what it reports with a baseline run of the same
copy: the case passes when the new findings are exactly the ones expected -
the rule, the file and a fragment of the message - and nothing else. The two
negative controls (I14, I15) pass only when nothing new is reported, because a
check that fails everything is as useless as one that fails nothing.

Injections aim by file and content, never by line number: an injection that
aims by position rots (tests/mcp2210 learned that), so every one of them
appends to a file or creates one.

Python 3 and nothing else; no BitBake. Level B (the effective dependency
graph) has its own injections, run by hand against the kas build, and is not
repeated here.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SCRIPT = os.path.join(REPO, 'scripts', 'med-layer-check.py')
LINE = re.compile(r'^(F\d|EX) (\S+):(\d+): (.*)$')

APP_SVC = 'meta-med-app/recipes-apps/eeg-acquisition-service/eeg-acquisition-service_1.0.0.bb'
FW = 'meta-med-framework/recipes-framework/med-framework-api/med-framework-api_1.0.0.bb'
FW_SRC = 'meta-med-framework/recipes-framework/med-framework-api/files/src'
FW_INC = 'meta-med-framework/recipes-framework/med-framework-api/files/include'
IMG_BASE = 'meta-med-distro/recipes-core/images/med-image-base.bb'
RAUC = 'meta-med-distro/recipes-core/rauc/rauc-conf.bbappend'


def append(root, rel, text):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'a') as fh:
        fh.write(text)


def remove_line(root, rel, snippet):
    path = os.path.join(root, rel)
    with open(path) as fh:
        lines = fh.readlines()
    kept = [l for l in lines if snippet not in l]
    assert len(kept) < len(lines), 'snippet %r not in %s' % (snippet, rel)
    with open(path, 'w') as fh:
        fh.writelines(kept)


def first_file(root, rel_dir, suffix):
    for f in sorted(os.listdir(os.path.join(root, rel_dir))):
        if f.endswith(suffix):
            return os.path.join(rel_dir, f)
    raise AssertionError('no *%s in %s' % (suffix, rel_dir))


def run(root, script=SCRIPT):
    p = subprocess.run([sys.executable, script, '--root', root,
                        '--layers-dir', os.path.join(REPO, 'layers')],
                       capture_output=True, text=True)
    found = set()
    warnings = []
    for l in p.stdout.splitlines():
        m = LINE.match(l)
        if m:
            found.add((m.group(1), m.group(2), m.group(4)))
        elif l.startswith('warning: '):
            warnings.append(l)
    return p.returncode, found, warnings


# (id, rule, description, inject(root, workdir) -> script path or None,
#  expected: list of (rule, file, message fragment), or [] for a control,
#  expected warning fragment or None)
CASES = [
    ('I1', 'F1', 'distro depends on app',
     lambda r, w: append(r, 'meta-med-distro/conf/layer.conf',
                         '\nLAYERDEPENDS_meta-med-distro += "meta-med-app"\n'),
     [('F1', 'meta-med-distro/conf/layer.conf', "depends on 'meta-med-app' (rank 4)")], None),
    ('I2', 'F1', 'BSP depends on distro',
     lambda r, w: append(r, 'meta-med-bsp/conf/layer.conf',
                         '\nLAYERDEPENDS_meta-med-bsp += "meta-med-distro"\n'),
     [('F1', 'meta-med-bsp/conf/layer.conf', "depends on 'meta-med-distro' (rank 2)")], None),
    ('I3', 'F2', 'framework depends on the AFE adjunct',
     lambda r, w: append(r, 'meta-med-framework/conf/layer.conf',
                         '\nLAYERDEPENDS_meta-med-framework += "meta-med-afe-ads1299"\n'),
     # the adjunct's name carries the part number, so F7 fires too (rule 9)
     [('F2', 'meta-med-framework/conf/layer.conf', "depends on adjunct 'meta-med-afe-ads1299'"),
      ('F7', 'meta-med-framework/conf/layer.conf', "part number 'ads1299'")], None),
    ('I3b', 'F2', 'desktop adjunct depends on app',
     lambda r, w: append(r, 'meta-med-desktop/conf/layer.conf',
                         '\nLAYERDEPENDS_meta-med-desktop += "meta-med-app"\n'),
     [('F2', 'meta-med-desktop/conf/layer.conf', "adjunct 'meta-med-desktop' depends on 'meta-med-app'")], None),
    ('I4', 'F3', 'BSP bbappends an application',
     lambda r, w: append(r, 'meta-med-bsp/recipes-apps/eeg-acquisition-service/'
                            'eeg-acquisition-service_%.bbappend', ''),
     [('F3', 'meta-med-bsp/recipes-apps/eeg-acquisition-service/eeg-acquisition-service_%.bbappend',
       "bbappend onto 'eeg-acquisition-service' from meta-med-app")], None),
    ('I5', 'F3', 'AFE adjunct bbappends the framework',
     lambda r, w: append(r, 'meta-med-afe-ads1299/recipes-framework/med-framework-api_%.bbappend', ''),
     [('F3', 'meta-med-afe-ads1299/recipes-framework/med-framework-api_%.bbappend',
       "bbappend onto 'med-framework-api' from meta-med-framework")], None),
    ('I6', 'F5', 'application RDEPENDS on the bridge module (F4 at level B)',
     lambda r, w: append(r, APP_SVC, '\nRDEPENDS:${PN} += "kernel-module-hid-mcp2210"\n'),
     [('F5', APP_SVC, "RDEPENDS gains 'kernel-module-hid-mcp2210'"),
      ('F7', APP_SVC, "part number 'mcp2210'")], None),
    # The plan's own I7: the BSP recipe's name carries a bootloader name, so
    # level A sees it by F6 in the distro conf even though F4 is level B.
    ('I7', 'F4', 'a BSP recipe through a variable (seen by F6)',
     lambda r, w: (append(r, 'meta-med-distro/conf/distro/med-os.conf',
                          '\nMED_X = "med-uboot-env-config"\n'),
                   append(r, FW, '\nRDEPENDS:${PN} += "${MED_X}"\n')),
     [('F6', 'meta-med-distro/conf/distro/med-os.conf', "'uboot' (bootloader)")],
     "'${MED_X}' is not expandable as text"),
    # The pure case: framework -> application through a variable. No pattern
    # names it; level A can only warn, and F4 is level B's to fail.
    ('I7b', 'F4', 'framework -> application through a variable: A only warns',
     lambda r, w: (append(r, 'meta-med-distro/conf/distro/med-os.conf',
                          '\nMED_Y = "eeg-hmi-gui"\n'),
                   append(r, FW, '\nRDEPENDS:${PN} += "${MED_Y}"\n')),
     [], "'${MED_Y}' is not expandable as text"),
    ('I8', 'F5', 'application DEPENDS on libiio',
     lambda r, w: append(r, APP_SVC, '\nDEPENDS += "libiio"\n'),
     [('F5', APP_SVC, "DEPENDS gains 'libiio'")], None),
    ('I9', 'F5', 'framework DEPENDS on zlib',
     lambda r, w: append(r, FW, '\nDEPENDS += "zlib"\n'),
     [('F5', FW, "framework DEPENDS gains 'zlib'")], None),
    ('I10', 'F6', 'QB_KERNEL_ROOT = /dev/vda2 back in the distro',
     lambda r, w: append(r, IMG_BASE, '\nQB_KERNEL_ROOT = "/dev/vda2"\n'),
     [('F6', IMG_BASE, "'/dev/vda2' (disk node)")], None),
    ('I11', 'F6', 'a wic layout naming grub-efi in the distro',
     lambda r, w: append(r, 'meta-med-distro/wic/x.wks',
                         'part /boot --source bootimg-efi --sourceparams="loader=grub-efi" --label boot\n'),
     [('F6', 'meta-med-distro/wic/x.wks', 'a wic layout above meta-med-bsp'),
      ('F6', 'meta-med-distro/wic/x.wks', "'bootimg-efi' (bootloader)"),
      ('F6', 'meta-med-distro/wic/x.wks', "'grub-efi' (bootloader)")], None),
    ('I12', 'F6', 'console=ttySTM0 in an application unit',
     lambda r, w: append(r, 'meta-med-app/recipes-apps/eeg-acquisition-service/files/x.service',
                         '[Service]\nEnvironment=CONSOLE=console=ttySTM0\n'),
     [('F6', 'meta-med-app/recipes-apps/eeg-acquisition-service/files/x.service',
       "'ttySTM0' (serial console)")], None),
    ('I13', 'F6', '/dev/mmcblk1p8 in framework code',
     lambda r, w: append(r, FW_SRC + '/MedicalStorage.cpp',
                         '\nstatic const char* kInjected = "/dev/mmcblk1p8";\n'),
     [('F6', FW_SRC + '/MedicalStorage.cpp', "'mmcblk1p8' (disk node)"),
      ('F9', FW_SRC + '/MedicalStorage.cpp', "'mmcblk1p8' in code")], None),
    ('I14', 'F6', 'control: a policy partition name in a distro script',
     lambda r, w: append(r, 'meta-med-distro/recipes-core/med-data-volume/files/med-data-provision.sh',
                         '\nINJECTED=/dev/disk/by-partlabel/med-data\n'),
     [], None),
    ('I15', 'F6', 'control: a bootloader named in a comment',
     lambda r, w: append(r, IMG_BASE, '\n# naming a bootloader such as grub here is forbidden\n'),
     [], None),
    ('I16', 'F7', 'part number in a framework comment',
     lambda r, w: append(r, FW_INC + '/MedicalDevice.h', '\n// an ADS1299 has 8 channels\n'),
     [('F7', FW_INC + '/MedicalDevice.h', "part number 'ADS1299'")], None),
    ('I17', 'F7', 'part number in an application DESCRIPTION',
     lambda r, w: append(r, 'meta-med-app/recipes-core/images/med-image-tomograph.bb',
                         '\nSUMMARY = "reads an MCP2210"\n'),
     [('F7', 'meta-med-app/recipes-core/images/med-image-tomograph.bb', "part number 'MCP2210'")], None),
    ('I18', 'EX', 'the line an exception covers is gone',
     lambda r, w: remove_line(r, RAUC, 'uboot|barebox)'),
     [('EX', RAUC, 'matches nothing - unused, remove it')], None),
    ('I19', 'EX', 'an exception with an empty reason',
     'empty-reason',
     [('EX', RAUC, 'has no reason')], None),
    ('I20', 'F8', 'a driver named after a part',
     lambda r, w: append(r, FW_SRC + '/MedicalDevice.cpp',
                         '\nstatic int x = (MedicalDeviceFactory::registerDriver("max30001", nullptr), 0);\n'),
     [('F8', FW_SRC + '/MedicalDevice.cpp', "driver 'max30001' is not a declared transport")], None),
    ('I21', 'F9', 'an enumerated node in a comment of an adjunct, no marker',
     lambda r, w: append(r, 'meta-med-desktop/conf/layer.conf', '\n# the card was mmcblk0 today\n'),
     [('F9', 'meta-med-desktop/conf/layer.conf', "'mmcblk0' in a comment without")], None),
    ('I22', 'F9', 'control: the same comment with the measurement marker',
     lambda r, w: append(r, 'meta-med-desktop/conf/layer.conf',
                         '\n# measured: 2026-11-02, the card was\n# mmcblk0 today\n'),
     [], None),
    ('I23', 'F5', 'an unclassified application recipe',
     lambda r, w: append(r, 'meta-med-app/recipes-apps/newapp/newapp_1.0.bb', 'DEPENDS = "qtbase"\n'),
     [('F5', 'meta-med-app/recipes-apps/newapp/newapp_1.0.bb', "'newapp' is unclassified")], None),
    ('I24', 'F1', 'an unclassified meta-med layer',
     lambda r, w: append(r, 'meta-med-extra/conf/layer.conf',
                         'BBFILE_COLLECTIONS += "meta-med-extra"\nLAYERDEPENDS_meta-med-extra = "core"\n'),
     [('F1', 'meta-med-extra', 'neither a stack layer nor a declared adjunct')], None),
]


def main():
    work = tempfile.mkdtemp(prefix='med-layer-check-')
    failures = 0
    try:
        base_root = os.path.join(work, 'base')
        shutil.copytree(os.path.join(REPO, 'meta-custom'), base_root, symlinks=True)
        _, baseline, base_warn = run(base_root)
        for cid, rule, desc, inject, expected, want_warning in CASES:
            root = os.path.join(work, cid)
            shutil.copytree(base_root, root, symlinks=True)
            script = SCRIPT
            if inject == 'empty-reason':
                script = os.path.join(work, cid + '-check.py')
                with open(SCRIPT) as fh:
                    src = fh.read()
                a = src.index("'reason':", src.index('EXCEPTIONS = ['))
                b = src.index("'date':", a)
                patched = src[:a] + "'reason': '   ',\n        " + src[b:]
                with open(script, 'w') as fh:
                    fh.write(patched)
            else:
                inject(root, work)
            code, found, warns = run(root, script)
            new = found - baseline
            new_warn = [w for w in warns if w not in base_warn]
            missing = [e for e in expected
                       if not any(r == e[0] and f == e[1] and e[2] in m for r, f, m in new)]
            extra = [n for n in new
                     if not any(n[0] == e[0] and n[1] == e[1] and e[2] in n[2] for e in expected)]
            ok = not missing and not extra
            if want_warning:
                ok = ok and any(want_warning in w for w in new_warn)
            if expected:
                ok = ok and code != 0
            status = 'ok  ' if ok else 'FAIL'
            if not ok:
                failures += 1
            what = ('%d new finding(s)' % len(new)) if expected else 'nothing new (control)'
            print('%s %-4s %-3s %s - %s' % (status, cid, rule, desc, what))
            for n in sorted(new):
                print('        %s %s: %s' % n)
            for w in new_warn:
                print('        %s' % w)
            for m in missing:
                print('        MISSING %s %s: ...%s...' % m)
            for x in extra:
                print('        UNEXPECTED %s %s: %s' % x)
            shutil.rmtree(root)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('layer-check fault injection: %d cases, %d failed' % (len(CASES), failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
