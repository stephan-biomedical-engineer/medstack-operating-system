#!/usr/bin/env python3
"""MedPlatform layer-boundary check, level A (static, no BitBake).

WHY THIS EXISTS
The thesis says the boundaries between the MedStack layers are enforced and
verified during the build. Until this script, nothing verified them: CLAUDE.md
rule 1 said so itself ("no build will report it"), and the rules it states had
already degraded on their own - rule 9 (no part number in the application or
the framework) was broken in committed code for about two weeks before anyone
looked, which is the same shape as the two violations of August 2026
(QB_KERNEL_ROOT = "/dev/vda2" and loader=grub-efi in the distro layer) that
only surfaced when a second machine needed a different answer.

A rule nothing checks is a recommendation. This turns the rules into a test
that fails, and every rule in it has been seen failing against an injected
violation before being believed (docs/RESULTS.md, the layer-check section).

WHAT IT CHECKS (ids are stable; the thesis cites them)
  F1  a stack layer's LAYERDEPENDS names only lower-ranked or external layers
  F2  no stack layer names an adjunct; an adjunct names only what it is allowed
  F3  no bbappend onto a MedPlatform recipe of a higher layer (or, from an
      adjunct or the BSP, onto any MedPlatform recipe)
  F5  the platform's own applications depend on the framework and Qt only, and
      the framework on libsystemd, libcrypto and its own ABI only
  F6  no machine, bootloader, serial console, disk node or wic layout above
      meta-med-bsp (code only; occurrences in comments are counted, not failed)
  F7  no part number in meta-med-app/meta-med-framework, comments included;
      in meta-med-distro only document names and the adjunct layer's name; in
      meta-med-bsp also the kernel's own symbol and module names
  F8  every framework driver is named after a transport, never after a part
  F9  no enumerated device name (mmcblkN, rpmsgN, remoteprocN, ...) anywhere,
      the BSP included, comments included - except in a comment that records
      a measurement and says so with the marker "measured:"
F4 (the effective dependency graph) needs BitBake and is level B,
scripts/med-layer-graph.py. kas/*.yml is out of scope by definition: those
files are required to name the machine and carry per-target values.

WHAT IT DOES NOT SEE
Text only. A dependency that arrives through a variable it cannot expand is
reported as a warning and left to level B; machine knowledge written without
any of the patterns below passes; a value injected by a kas project file is
out of scope. docs/RESULTS.md lists these as limitations.

Usage:  make check-layers
        python3 scripts/med-layer-check.py [--root DIR] [--layers-dir DIR] [-v]
          --root        the meta-custom directory to check (default: this repo's)
          --layers-dir  where the third-party layers are, to learn machine names
                        (repeatable)
          -v            also list the F6 occurrences found in comments
"""

import argparse
import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# The layers. Rank orders the stack (a layer may consume only lower ranks);
# adjuncts are outside the stack and consume only what is listed for them.
# A meta-med-* directory missing from this table fails the check: a new layer
# is a decision about where it sits, and that decision is written here.
# ---------------------------------------------------------------------------
STACK = {
    'meta-med-bsp': 1,
    'meta-med-distro': 2,
    'meta-med-framework': 3,
    'meta-med-app': 4,
}
ADJUNCT_MAY_DEPEND = {
    # med-amp-abi, the header-only wire format both the framework and the
    # co-processor firmware consume (the layer's own layer.conf says why).
    'meta-med-afe-ads1299': {'meta-med-framework'},
    # The host station's image requires the EEG image.
    'meta-med-host': {'meta-med-app'},
    'meta-med-desktop': set(),
}
ABOVE_BSP = ('meta-med-distro', 'meta-med-framework', 'meta-med-app')

# F5 - rule 6. Every recipe under meta-med-app/recipes-apps is either one of
# the platform's own applications or third-party software packaged unmodified;
# an unclassified one fails, so the question is asked when it is added.
OWN_APPS = {'eeg-acquisition-service', 'eeg-hmi-gui'}
THIRD_PARTY_APPS = {
    'edfbrowser': 'EDFbrowser 2.15, packaged unmodified; not a MedFramework client',
    'med-review': 'launcher and sandbox for edfbrowser; links nothing',
}
OWN_APP_DEPENDS = re.compile(r'^(med-framework-api|qt[a-z0-9][a-z0-9-]*)$')
OWN_APP_RDEPENDS = re.compile(r'^(libmedframework\d*|med-framework-api|qt[a-z0-9][a-z0-9-]*)$')
FRAMEWORK_DEPENDS = {'systemd', 'openssl', 'med-amp-abi'}

# F8 - the framework's drivers, by the transport they speak.
TRANSPORTS = {
    'simulated': 'no transport: the framework generates the samples',
    'rpmsg': 'the OpenAMP rpmsg channel to a co-processor',
    'iio': 'the kernel IIO ABI',
}

# ---------------------------------------------------------------------------
# Exceptions. Same rule as EXPECTED_FAILED in scripts/med-check.py: an
# allowlist nobody justifies is a muted alarm. Each entry is one file and one
# literal snippet of the offending line, never a directory; the reason may not
# be empty; and an entry that no longer matches anything fails the check, so
# the list cannot only grow.
# ---------------------------------------------------------------------------
EXCEPTIONS = [
    {
        'rule': 'F6',
        'path': 'meta-med-distro/recipes-core/rauc/rauc-conf.bbappend',
        'snippet': 'uboot|barebox)',
        'reason': "branches on MED_BOOTLOADER, RAUC's backend name, which "
                  "meta-med-bsp answers per machine; the distro never chooses "
                  "the bootloader, it validates the backend's slot syntax",
        'date': '2026-11-02',
    },
]

# ---------------------------------------------------------------------------
# Patterns
# ---------------------------------------------------------------------------
STATIC_MACHINES = ['qemux86-64', 'qemux86', 'qemuarm64', 'qemuarm',
                   'stm32mp25-disco', 'stm32mp2', 'raspberrypi']


def machine_names(layers_dirs):
    if isinstance(layers_dirs, str):
        layers_dirs = [layers_dirs]
    names = set()
    for layers_dir in layers_dirs or []:
        if not os.path.isdir(layers_dir):
            continue
        for dirpath, _dirs, files in os.walk(layers_dir):
            if os.path.basename(dirpath) != 'machine' or \
               os.path.basename(os.path.dirname(dirpath)) != 'conf':
                continue
            names.update(f[:-5] for f in files if f.endswith('.conf'))
    source = ('conf/machine/*.conf under %s' % ', '.join(layers_dirs)) if names else 'a static list'
    names.update(STATIC_MACHINES)
    return sorted(names, key=len, reverse=True), source


def f6_patterns(machines):
    word = r'(?<![A-Za-z0-9_-])%s(?![A-Za-z0-9_])'
    return [
        ('machine', re.compile(word % '(?:%s)' % '|'.join(map(re.escape, machines)))),
        ('SoC', re.compile(r'(?<![A-Za-z0-9_])stm32mp\d+\w*', re.I)),
        ('bootloader', re.compile(
            r'(?<![A-Za-z0-9_])(?:grub(?:-efi)?|u-?boot|barebox|extlinux|'
            r'systemd-boot|bootimg-efi|bootimg-partition)(?![A-Za-z0-9_])', re.I)),
        ('serial console', re.compile(r'(?<![A-Za-z0-9_])(?:tty(?:S|STM|AMA|mxc|PS|O)\d+|hvc\d+)')),
        ('disk node', re.compile(
            r'/dev/(?:vd|sd|hd|xvd)[a-z]+\d*(?![A-Za-z0-9_])|'
            r'(?<![A-Za-z0-9_])(?:mmcblk\d+(?:p\d+)?|nvme\d+n\d+(?:p\d+)?)|'
            r'/dev/mtd(?:block)?\d+')),
        # med-* names are policy (meta-med-distro names the slots and /data)
        ('disk name', re.compile(r'by-(?:partlabel|partuuid|label|uuid|path|id)/(?!med-)[^\s"\')]+')),
    ]


PART = re.compile(r'ads129[0-9x]|mcp2210', re.I)
F7_ALLOWED = {
    # what may be said about a part, per layer, before the check looks
    'meta-med-distro': [re.compile(r'[\w./-]*\.md\b'), re.compile(r'meta-med-afe-ads1299')],
    'meta-med-bsp': [re.compile(r'[\w./-]*\.md\b'), re.compile(r'meta-med-afe-ads1299'),
                     # the kernel's names, not ours: a Kconfig symbol, a module
                     re.compile(r'(?:CONFIG_)?(?:TI_ADS1299|HID_MCP2210)'),
                     re.compile(r'(?:kernel-module-)?(?:ti-ads1299|hid-mcp2210)')],
}
F7_STRICT = ('meta-med-app', 'meta-med-framework')

ENUMERATED = re.compile(
    r'(?<![A-Za-z0-9_])(?:mmcblk\d+(?:p\d+)?|rpmsg(?:_ctrl)?\d+|remoteproc\d+|'
    r'iio:device\d+|hidraw\d+)(?![A-Za-z0-9_])')
MEASURED = 'measured:'
MEASURED_REACH = 3   # the marker may open a sentence wrapped over a few lines

DRIVER_REGISTRATION = re.compile(r'registerDriver\(\s*"([^"]+)"')

# ---------------------------------------------------------------------------
# Comment classification, by language. Each line becomes (text, mask) where
# mask[i] is True when character i is inside a comment, so "code; ///< note"
# is code up to the comment and comment after it.
# ---------------------------------------------------------------------------
C_LIKE = {'.h', '.hpp', '.c', '.cc', '.cpp', '.qml', '.js', '.dts', '.dtsi', '.dtso'}
PROSE = {'.md', '.rst'}


def language(path):
    name = os.path.basename(path)
    if name == 'CMakeLists.txt':
        return 'hash'
    stem, ext = os.path.splitext(name)
    if ext == '.in':
        ext = os.path.splitext(stem)[1]
    if ext in C_LIKE:
        return 'c'
    if ext in PROSE or ext == '.txt':
        return 'prose'
    return 'hash'


def classify(lines, lang):
    out = []
    if lang == 'prose':
        return [(t, [True] * len(t)) for t in lines]
    if lang == 'hash':
        for t in lines:
            s = t.lstrip()
            out.append((t, [s.startswith('#')] * len(t)))
        return out
    in_block = False
    for t in lines:
        mask = [False] * len(t)
        i, quote = 0, None
        while i < len(t):
            if in_block:
                mask[i] = True
                if t.startswith('*/', i):
                    mask[i + 1] = True
                    in_block = False
                    i += 2
                    continue
                i += 1
            elif quote:
                if t[i] == '\\':
                    i += 2
                    continue
                if t[i] == quote:
                    quote = None
                i += 1
            elif t.startswith('//', i):
                for j in range(i, len(t)):
                    mask[j] = True
                break
            elif t.startswith('/*', i):
                in_block = True
                mask[i] = True
                i += 1
            elif t[i] in '"\'':
                quote = t[i]
                i += 1
            else:
                i += 1
        out.append((t, mask))
    return out


def is_comment_line(text, mask):
    s = text.strip()
    return bool(s) and all(m for c, m in zip(text, mask) if not c.isspace())


# ---------------------------------------------------------------------------
# BitBake text
# ---------------------------------------------------------------------------
ASSIGN = re.compile(r'^\s*(?:export\s+)?([A-Za-z0-9_${}:/.+-]+?)\s*'
                    r'(\?\?=|\?=|:=|\+=|=\+|\.=|=\.|=)\s*"(.*)"\s*$', re.S)


def logical_lines(lines):
    """Join backslash continuations; yield (first line number, text)."""
    buf, start = [], None
    for n, t in enumerate(lines, 1):
        if start is None:
            start = n
        if t.rstrip().endswith('\\'):
            buf.append(t.rstrip()[:-1])
            continue
        buf.append(t)
        yield start, ' '.join(buf)
        buf, start = [], None
    if buf:
        yield start, ' '.join(buf)


def assignments(lines):
    for n, t in logical_lines(lines):
        if t.lstrip().startswith('#'):
            continue
        m = ASSIGN.match(t)
        if m:
            yield n, m.group(1), m.group(2), m.group(3)


def dep_items(value):
    return [w for w in value.split() if w]


def pn_of(filename):
    base = filename.rsplit('.', 1)[0]
    if '_' in base:
        pn, ver = base.rsplit('_', 1)
        return pn, ver
    return base, None


# ---------------------------------------------------------------------------
class Check:
    def __init__(self, root, layers_dir, verbose):
        self.root = root
        self.verbose = verbose
        self.violations = []      # (rule, path, line, message)
        self.warnings = []
        self.comment_hits = {}    # F6 occurrences in comments, per layer
        self.comment_list = []
        self.used_exceptions = set()
        self.machines, self.machine_source = machine_names(layers_dir)
        self.f6 = f6_patterns(self.machines)
        self.layers = sorted(d for d in os.listdir(root)
                             if d.startswith('meta-med') and os.path.isdir(os.path.join(root, d)))
        self.files = {}           # layer -> [relative paths]
        for layer in self.layers:
            acc = []
            for dirpath, dirs, files in os.walk(os.path.join(root, layer)):
                dirs[:] = sorted(d for d in dirs if d not in ('.git', '__pycache__'))
                for f in sorted(files):
                    acc.append(os.path.relpath(os.path.join(dirpath, f), root))
            self.files[layer] = acc
        self._text = {}

    # -- helpers ---------------------------------------------------------
    def read(self, rel):
        if rel not in self._text:
            try:
                with open(os.path.join(self.root, rel), 'rb') as fh:
                    raw = fh.read()
            except OSError:
                raw = b''
            self._text[rel] = None if b'\0' in raw else raw.decode('utf-8', 'replace').splitlines()
        return self._text[rel]

    def excepted(self, rule, rel, text):
        for i, e in enumerate(EXCEPTIONS):
            if e['rule'] == rule and e['path'] == rel and e['snippet'] in text:
                self.used_exceptions.add(i)
                return True
        return False

    def fail(self, rule, rel, line, msg, text=''):
        if text and self.excepted(rule, rel, text):
            return
        self.violations.append((rule, rel, line, msg))

    def layer_of(self, rel):
        return rel.split(os.sep, 1)[0]

    # -- the rules ---------------------------------------------------------
    def classify_layers(self):
        for layer in self.layers:
            if layer not in STACK and layer not in ADJUNCT_MAY_DEPEND:
                self.fail('F1', layer, 0, 'layer %s is neither a stack layer nor a declared '
                          'adjunct; classify it in STACK or ADJUNCT_MAY_DEPEND' % layer)

    def check_layerdepends(self):
        for layer in self.layers:
            rel = os.path.join(layer, 'conf', 'layer.conf')
            lines = self.read(rel)
            if lines is None or not lines:
                self.fail('F1', rel, 0, 'no conf/layer.conf')
                continue
            colls, deps = [], []
            for n, name, op, value in assignments(lines):
                if name == 'BBFILE_COLLECTIONS':
                    colls += dep_items(value)
                elif name.startswith('LAYERDEPENDS_') or name.startswith('LAYERDEPENDS:'):
                    deps.append((n, re.sub(r'\([^)]*\)', ' ', value)))
            if colls and colls != [layer]:
                self.warnings.append('%s: collection %s differs from its directory name' % (rel, colls))
            for n, value in deps:
                for dep in dep_items(value):
                    if not dep.startswith('meta-med'):
                        continue
                    if layer in STACK:
                        if dep in ADJUNCT_MAY_DEPEND:
                            self.fail('F2', rel, n, "'%s' depends on adjunct '%s' - no stack layer may "
                                      'consume an adjunct' % (layer, dep), dep)
                        elif dep in STACK and STACK[dep] >= STACK[layer]:
                            self.fail('F1', rel, n, "'%s' (rank %d) depends on '%s' (rank %d) - a stack "
                                      'layer consumes only lower ranks' %
                                      (layer, STACK[layer], dep, STACK[dep]), dep)
                        elif dep not in STACK:
                            self.fail('F1', rel, n, "'%s' depends on unclassified '%s'" % (layer, dep), dep)
                    elif layer in ADJUNCT_MAY_DEPEND:
                        if dep not in ADJUNCT_MAY_DEPEND[layer]:
                            allowed = ', '.join(sorted(ADJUNCT_MAY_DEPEND[layer])) or 'nothing'
                            self.fail('F2', rel, n, "adjunct '%s' depends on '%s' - it may consume "
                                      '%s' % (layer, dep, allowed), dep)

    def check_bbappends(self):
        recipes = {}   # pn -> [(layer, version)]
        for layer in self.layers:
            for rel in self.files[layer]:
                if rel.endswith('.bb'):
                    pn, ver = pn_of(os.path.basename(rel))
                    recipes.setdefault(pn, []).append((layer, ver))
        for layer in self.layers:
            for rel in self.files[layer]:
                if not rel.endswith('.bbappend'):
                    continue
                pn, ver = pn_of(os.path.basename(rel))
                for target, tver in recipes.get(pn, []):
                    if ver not in (None, '%') and tver is not None and \
                       not re.fullmatch(re.escape(ver).replace('%', '.*'), tver):
                        continue
                    if target == layer:
                        continue
                    ok = layer in STACK and target in STACK and STACK[target] < STACK[layer]
                    if not ok:
                        self.fail('F3', rel, 1, "bbappend onto '%s' from %s - %s may append only "
                                  '%s' % (pn, target, layer,
                                          'external or lower-ranked recipes' if layer in STACK and layer != 'meta-med-bsp'
                                          else 'external recipes'), os.path.basename(rel))

    def check_rule6(self):
        apps_dir = os.path.join('meta-med-app', 'recipes-apps') + os.sep
        for layer in ('meta-med-app', 'meta-med-framework'):
            for rel in self.files.get(layer, []):
                if not rel.endswith('.bb'):
                    continue
                pn, _ = pn_of(os.path.basename(rel))
                if layer == 'meta-med-app':
                    if not rel.startswith(apps_dir):
                        continue
                    if pn in THIRD_PARTY_APPS:
                        continue
                    if pn not in OWN_APPS:
                        self.fail('F5', rel, 1, "application recipe '%s' is unclassified - add it to "
                                  'OWN_APPS or THIRD_PARTY_APPS (with a reason)' % pn)
                        continue
                for n, name, op, value in assignments(self.read(rel) or []):
                    parts = name.split(':')
                    base, ovr = parts[0], parts[1:]
                    if base == 'DEPENDS' and set(ovr) <= {'append', 'prepend', 'class-target'}:
                        kind = 'DEPENDS'
                    elif base == 'RDEPENDS' and ovr[:1] == ['${PN}'] and set(ovr[1:]) <= {'append', 'prepend'}:
                        kind = 'RDEPENDS'
                    elif base.startswith('RDEPENDS_${PN}'):
                        kind = 'RDEPENDS'
                    else:
                        continue
                    for item in dep_items(value):
                        if '${' in item:
                            self.warnings.append('F5 %s:%d: %s item %r is not expandable as text - '
                                                 'verified at level B' % (rel, n, kind, item))
                            continue
                        if layer == 'meta-med-framework':
                            if kind == 'DEPENDS' and item not in FRAMEWORK_DEPENDS:
                                self.fail('F5', rel, n, "framework DEPENDS gains '%s' - the framework "
                                          'depends on %s only' % (item, ', '.join(sorted(FRAMEWORK_DEPENDS))),
                                          item)
                        else:
                            allowed = OWN_APP_DEPENDS if kind == 'DEPENDS' else OWN_APP_RDEPENDS
                            if not allowed.match(item):
                                self.fail('F5', rel, n, "%s %s gains '%s' - an application links the "
                                          'framework and Qt only' % (pn, kind, item), item)

    def scan_text(self):
        for layer in self.layers:
            for rel in self.files[layer]:
                lines = self.read(rel)
                if lines is None:
                    continue
                lang = language(rel)
                cls = classify(lines, lang)
                if layer in ABOVE_BSP:
                    base = os.path.basename(rel)
                    if base.endswith('.wks') or base.endswith('.wks.in'):
                        self.fail('F6', rel, 1, 'a wic layout above meta-med-bsp - a disk layout is '
                                  'placement, only meta-med-bsp may hold one')
                    self.scan_f6(layer, rel, cls)
                if layer in F7_STRICT or layer in F7_ALLOWED:
                    self.scan_f7(layer, rel, cls)
                self.scan_f9(rel, cls)
                if layer == 'meta-med-framework' and lang == 'c':
                    self.scan_f8(rel, cls)

    def scan_f6(self, layer, rel, cls):
        for n, (text, mask) in enumerate(cls, 1):
            for label, rx in self.f6:
                for m in rx.finditer(text):
                    if mask[m.start()]:
                        self.comment_hits[layer] = self.comment_hits.get(layer, 0) + 1
                        self.comment_list.append('%s:%d: %r (%s)' % (rel, n, m.group(0), label))
                    else:
                        self.fail('F6', rel, n, "'%s' (%s) - only meta-med-bsp may name this" %
                                  (m.group(0), label), text)

    def scan_f7(self, layer, rel, cls):
        for n, (text, _mask) in enumerate(cls, 1):
            probe = text
            for rx in F7_ALLOWED.get(layer, []):
                probe = rx.sub(lambda m: '_' * len(m.group(0)), probe)
            for m in PART.finditer(probe):
                where = ('anywhere in this layer, comments included' if layer in F7_STRICT
                         else 'here except as %s' % ('a document name or the adjunct layer\'s name'
                                                     if layer == 'meta-med-distro'
                                                     else "a document, the adjunct layer, or the kernel's own symbol/module name"))
                self.fail('F7', rel, n, "part number '%s' - not allowed %s" %
                          (text[m.start():m.end()], where), text)
                break

    def scan_f8(self, rel, cls):
        for n, (text, mask) in enumerate(cls, 1):
            for m in DRIVER_REGISTRATION.finditer(text):
                if mask[m.start()]:
                    continue
                name = m.group(1)
                if name not in TRANSPORTS:
                    self.fail('F8', rel, n, "driver '%s' is not a declared transport - a driver is "
                              'named after the link it speaks, never after a part; declare it in '
                              'TRANSPORTS' % name, text)

    def scan_f9(self, rel, cls):
        for n, (text, mask) in enumerate(cls, 1):
            for m in ENUMERATED.finditer(text):
                if mask[m.start()] and self.marked(cls, n - 1):
                    continue
                kind = 'in a comment without "%s"' % MEASURED if mask[m.start()] else 'in code'
                self.fail('F9', rel, n, "enumerated device name '%s' %s - numbering is a race, "
                          'address the device by a name the platform gives it' % (m.group(0), kind),
                          text)

    def marked(self, cls, idx):
        for k in range(idx, max(-1, idx - MEASURED_REACH - 1), -1):
            text, mask = cls[k]
            if not is_comment_line(text, mask):
                return False
            if MEASURED in text.lower():
                return True
        return False

    def check_exceptions(self):
        for i, e in enumerate(EXCEPTIONS):
            label = '%s %s %r' % (e.get('rule'), e.get('path'), e.get('snippet'))
            if not str(e.get('reason', '')).strip():
                self.violations.append(('EX', e.get('path', '?'), 0,
                                        'exception %s has no reason - an allowlist entry nobody '
                                        'justifies is a muted alarm' % label))
            if not re.fullmatch(r'\d{4}-\d{2}-\d{2}', str(e.get('date', ''))):
                self.violations.append(('EX', e.get('path', '?'), 0, 'exception %s has no date' % label))
            if i not in self.used_exceptions:
                self.violations.append(('EX', e.get('path', '?'), 0,
                                        'exception %s matches nothing - unused, remove it' % label))

    # ---------------------------------------------------------------------
    def run(self):
        self.classify_layers()
        self.check_layerdepends()
        self.check_bbappends()
        self.check_rule6()
        self.scan_text()
        self.check_exceptions()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default=os.path.join(REPO, 'meta-custom'))
    ap.add_argument('--layers-dir', action='append',
                    help='where third-party layers are (repeatable); default: this repo\'s layers/')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    if not args.layers_dir:
        args.layers_dir = [os.path.join(REPO, 'layers')]
    t0 = time.monotonic()
    c = Check(os.path.abspath(args.root), args.layers_dir, args.verbose)
    c.run()
    elapsed = time.monotonic() - t0

    print('layer check (level A) over %s' % args.root)
    print('  layers: %s' % ', '.join(c.layers))
    print('  machine names: %d, from %s' % (len(c.machines), c.machine_source))
    print('  out of scope by definition: kas/*.yml (required to name the machine)')
    for rule, rel, line, msg in sorted(c.violations):
        print('%s %s:%d: %s' % (rule, rel, line, msg))
    for w in c.warnings:
        print('warning: %s' % w)
    if c.verbose:
        for h in c.comment_list:
            print('  F6 in comment: %s' % h)
    counts = {}
    for v in c.violations:
        counts[v[0]] = counts.get(v[0], 0) + 1
    print('summary: ' + ', '.join('%s %s' % (r, ('%d violation%s' % (counts[r], 's' if counts[r] > 1 else ''))
                                             if r in counts else 'ok')
                                  for r in ('F1', 'F2', 'F3', 'F5', 'F6', 'F7', 'F8', 'F9', 'EX')))
    print('F6 in comments (informative, not failed): %s' %
          (', '.join('%s %d' % kv for kv in sorted(c.comment_hits.items())) or 'none'))
    print('exceptions: %d, %.2f s' % (len(EXCEPTIONS), elapsed))
    return 1 if c.violations else 0


if __name__ == '__main__':
    sys.exit(main())
