#!/usr/bin/env python3
"""MedPlatform layer-boundary check, level B (what BitBake resolved).

WHY THIS EXISTS
Level A (scripts/med-layer-check.py) reads the layers' text. It cannot see a
dependency that arrives through a variable, a packagegroup or a class, and it
cannot say which layer's recipe BitBake actually chose for a name. This reads
what BitBake resolved for one profile - the task graph of `bitbake -g`, the
bbappends `bitbake-layers show-appends` applied, and the expanded values of
`bitbake -e` - and holds them to the same rules:

  F3  every bbappend BitBake applied from a MedPlatform layer targets an
      external recipe or a lower-ranked one (confirms level A by resolution)
  F4  every edge of the task graph between two MedPlatform recipes goes down
      the stack; an edge from the stack into an adjunct exists only from an
      image, a packagegroup or a bundle, and only for a recipe one of the hook
      variables names (MED_BSP_INSTALL, MED_AFE_INSTALL*, MED_AMP_FIRMWARE,
      MED_GPU_PACKAGES, MED_GUI_COMPOSITOR); an adjunct reaches the stack only
      as its own entry in med-layer-check.py's ADJUNCT_MAY_DEPEND allows
  F5  the expanded DEPENDS/RDEPENDS of the framework and of the platform's own
      applications, build tools aside, are what rule 6 allows
  F6  no variable of the image whose value names a machine, bootloader,
      console or disk node is decided by a file above meta-med-bsp, unless the
      value reaches it through a MED_* hook variable answered below

The layer tables (ranks, adjunct allowances, own applications, patterns) are
imported from med-layer-check.py, so the two levels cannot disagree on them.

Usage (inside the kas environment, from the build directory):
  python3 ../scripts/med-layer-graph.py collect <profile> <image>
  python3 ../scripts/med-layer-graph.py analyze [<profile> ...]
make check-layers-graph runs both over every profile.

WHAT IT DOES NOT SEE
A dependency that does not pass through BitBake (dlopen, a path mounted at
runtime); a value injected by a kas project file (out of scope by
definition); a profile that is not collected. docs/RESULTS.md lists these.
"""

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('medlayercheck', os.path.join(HERE, 'med-layer-check.py'))
A = importlib.util.module_from_spec(spec)
spec.loader.exec_module(A)

OUT = 'layer-graph'
OWN_RECIPES = sorted(A.OWN_APPS) + ['med-framework-api']
HOOKS = re.compile(r'^MED_(BSP_INSTALL|AFE_INSTALL\w*|AMP_FIRMWARE|GPU_PACKAGES|GUI_COMPOSITOR)$')
BUILD_TOOL = re.compile(r'(-native$|^virtual/|-cross(-|$)|^nativesdk-)')
# Variables a value above the BSP may refer to: they are the machine's own
# answer, by name, not a choice made in that layer.
GENERIC_REFS = {'MACHINE', 'MACHINE_ARCH', 'TUNE_PKGARCH', 'DEPLOY_DIR_IMAGE', 'IMAGE_LINK_NAME',
                'IMAGE_NAME', 'IMAGE_BASENAME', 'PN', 'IMGDEPLOYDIR', 'TOPDIR'}
LAYER_IN_PATH = re.compile(r'/meta-custom/(meta-med[\w-]*)/')


def layer_of(path):
    m = LAYER_IN_PATH.search(path or '')
    return m.group(1) if m else None


def kind(layer):
    if layer is None:
        return 'external'
    if layer in A.STACK:
        return 'stack'
    if layer in A.ADJUNCT_MAY_DEPEND:
        return 'adjunct'
    return 'unclassified'


# --------------------------------------------------------------------- collect
def collect(profile, image):
    dest = os.path.join(OUT, profile)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    t0 = time.monotonic()
    # bitbake -g only; never -n (it can delete a deployed image under a new config)
    subprocess.run(['bitbake', '-g', image], check=True, stdout=subprocess.DEVNULL)
    for f in ('task-depends.dot', 'pn-buildlist'):
        shutil.move(f, os.path.join(dest, f))
    with open(os.path.join(dest, 'appends.txt'), 'w') as fh:
        subprocess.run(['bitbake-layers', 'show-appends'], check=True, stdout=fh,
                       stderr=subprocess.DEVNULL)
    with open(os.path.join(dest, 'pn-buildlist')) as fh:
        built = set(fh.read().split())
    for r in [image] + [r for r in OWN_RECIPES if r in built]:
        with open(os.path.join(dest, 'env-%s.txt' % r), 'w') as fh:
            subprocess.run(['bitbake', '-e', r], check=True, stdout=fh)
    with open(os.path.join(dest, 'profile.txt'), 'w') as fh:
        fh.write('%s %s %.1f\n' % (profile, image, time.monotonic() - t0))
    print('collected %s (%s) in %.0f s' % (profile, image, time.monotonic() - t0))


# --------------------------------------------------------------------- parsing
NODE = re.compile(r'^"([^"]+)\.(do_\w+)" \[label="[^"]*\\n([^"\\]*)"\]')
EDGE = re.compile(r'^"([^"]+)\.(do_\w+)" -> "([^"]+)\.(do_\w+)"')
FINAL = re.compile(r'^(?:export )?([A-Za-z0-9_:${}.+/-]+)="(.*)"$')


def read_graph(path):
    files, edges = {}, set()
    with open(path) as fh:
        for line in fh:
            m = NODE.match(line)
            if m:
                files[m.group(1)] = m.group(3).split(':')[-1]
                continue
            m = EDGE.match(line)
            if m and m.group(1) != m.group(3):
                edges.add((m.group(1), m.group(2), m.group(3), m.group(4)))
    return files, edges


def read_appends(path):
    out, current = [], None
    with open(path) as fh:
        for line in fh:
            if re.match(r'^\S+\.bb:\s*$', line):
                current = line.strip()[:-1]
            elif line.startswith('  ') and current and line.strip().endswith('.bbappend'):
                out.append((current, line.strip()))
    return out


def read_env(path):
    """Final values and, per variable, its history: [(op, file, line, literal)]."""
    final, history, pre = {}, {}, {}
    var, ops, pending, in_pre = None, [], None, False
    with open(path, errors='replace') as fh:
        for raw in fh:
            line = raw.rstrip('\n')
            m = re.match(r'^# \$(\S+)', line)
            if m:
                var, ops, pending, in_pre = m.group(1), [], None, False
                continue
            if var and line.startswith('#   ') and not line.startswith('#     ') and not in_pre:
                m = re.match(r'^#   (\S+) (/\S+):(\d+)\s*$', line)
                if m:
                    pending = [m.group(1), m.group(2), int(m.group(3)), '']
                    ops.append(pending)
                continue
            if var and line.startswith('#     ') and pending is not None:
                pending[3] += line[6:]
                continue
            if var and line.startswith('# pre-expansion value:'):
                in_pre = True
                continue
            if var and in_pre and line.startswith('#   '):
                pre[var] = line[4:].strip()
                continue
            m = FINAL.match(line)
            if m:
                final[m.group(1)] = m.group(2)
                if var == m.group(1):
                    history[var] = ops
                var, ops, pending, in_pre = None, [], None, False
    return final, history, pre


# --------------------------------------------------------------------- analysis
def analyze(profile):
    base = os.path.join(OUT, profile)
    v = []
    info = []
    with open(os.path.join(base, 'profile.txt')) as fh:
        _, image, secs = fh.read().split()
    files, edges = read_graph(os.path.join(base, 'task-depends.dot'))
    pn_layer = {pn: layer_of(f) for pn, f in files.items()}
    img_final, img_hist, img_pre = read_env(os.path.join(base, 'env-%s.txt' % image))

    # names the hook variables carry, in this profile
    hooked = set()
    for name, value in img_final.items():
        if HOOKS.match(name):
            hooked.update(value.split())

    # F3 - what BitBake applied
    rec_layer = {}
    for pn, f in files.items():
        rec_layer[os.path.basename(f)] = layer_of(f)
    for recipe, bbappend in read_appends(os.path.join(base, 'appends.txt')):
        src = layer_of(bbappend)
        if src is None:
            continue
        tgt = rec_layer.get(recipe)
        if tgt is None and '/meta-custom/' not in recipe:
            # a recipe outside this image's graph; resolve by name only
            tgt = None
        if tgt is None or tgt == src:
            continue
        ok = kind(src) == 'stack' and kind(tgt) == 'stack' and A.STACK[tgt] < A.STACK[src]
        if not ok:
            v.append(('F3', '%s appends %s (%s)' % (os.path.relpath(bbappend.split('/meta-custom/')[-1]), recipe, tgt)))

    # F4 - every edge between MedPlatform recipes.
    #
    # Recursive tasks (do_rm_work_all, do_fetchall, ...) and an image's
    # do_rootfs depend on the whole closure, so they are not the recipe's own
    # dependencies: those edges are left out, and each recipe in the closure is
    # held to the rule by its own direct edges instead. What an image itself
    # may reach in an adjunct is what a hook variable names, and what those
    # recipes in turn depend on.
    images = {pn for pn, t, _, _ in edges if t == 'do_rootfs'}
    direct = {}
    for a, at, b, bt in edges:
        if at.endswith('_all') or a in images:
            continue
        direct.setdefault(a, set()).add(b)
    reach = set()
    stack_ = [n for n in hooked if n in files]
    while stack_:
        n = stack_.pop()
        if n in reach:
            continue
        reach.add(n)
        stack_.extend(direct.get(n, ()))
    seen = set()
    hook_edges = 0
    for a, at, b, bt in sorted(edges):
        la, lb = pn_layer.get(a), pn_layer.get(b)
        if la is None or lb is None or la == lb:
            continue
        if at.endswith('_all'):
            continue
        key = (a, b)
        if key in seen:
            continue
        seen.add(key)
        ka, kb = kind(la), kind(lb)
        bad = None
        if 'unclassified' in (ka, kb):
            bad = 'unclassified layer'
        elif a in images:
            # an aggregator: only stack -> adjunct needs a hook
            if ka == 'stack' and kb == 'adjunct':
                if b in reach:
                    hook_edges += 1
                else:
                    bad = 'image reaches an adjunct recipe no hook variable names'
        elif ka == 'stack' and kb == 'stack':
            if A.STACK[lb] > A.STACK[la]:
                bad = 'goes up the stack (%s rank %d -> %s rank %d)' % (la, A.STACK[la], lb, A.STACK[lb])
        elif ka == 'stack' and kb == 'adjunct':
            path = files.get(a, '')
            consumer = '/packagegroups/' in path or '/bundles/' in path
            if consumer and b in hooked:
                hook_edges += 1
            else:
                bad = ('stack -> adjunct outside an image or packagegroup' if not consumer
                       else 'stack -> adjunct not named by any hook variable')
        elif ka == 'adjunct':
            if kb == 'stack' and lb not in A.ADJUNCT_MAY_DEPEND[la]:
                bad = 'adjunct %s may consume %s only' % (
                    la, ', '.join(sorted(A.ADJUNCT_MAY_DEPEND[la])) or 'nothing')
            elif kb == 'adjunct':
                bad = 'adjunct -> adjunct'
        if bad:
            v.append(('F4', '%s (%s) %s -> %s (%s) %s: %s' % (a, la, at, b, lb, bt, bad)))

    # F5 - expanded dependencies of the platform's own code
    for r in OWN_RECIPES:
        envf = os.path.join(base, 'env-%s.txt' % r)
        if not os.path.exists(envf):
            continue
        final, _, _ = read_env(envf)
        for var in ('DEPENDS', 'RDEPENDS:%s' % r):
            items = [w for w in final.get(var, '').split() if not BUILD_TOOL.search(w)]
            for w in items:
                if r == 'med-framework-api':
                    ok = var != 'DEPENDS' or w in A.FRAMEWORK_DEPENDS
                else:
                    ok = (A.OWN_APP_DEPENDS if var == 'DEPENDS' else A.OWN_APP_RDEPENDS).match(w)
                if not ok:
                    v.append(('F5', '%s %s (expanded) contains %s' % (r, var, w)))
            info.append('F5 %s %s, build tools aside: %s' % (r, var, ' '.join(items) or '(empty)'))

    # F6 - values of the image decided above the BSP
    pats = A.f6_patterns(A.machine_names(os.path.join(os.path.dirname(os.path.abspath(OUT)), '..', 'layers'))[0])
    via_hook = 0
    for var, ops in img_hist.items():
        value = img_final.get(var, '')
        if not any(rx.search(value) for _, rx in pats):
            continue
        decisive = [o for o in ops if layer_of(o[1]) in A.ABOVE_BSP]
        for op, f, ln, literal in decisive:
            if any(rx.search(literal) for _, rx in pats):
                v.append(('F6', '%s: %s:%d sets it to a literal naming placement' %
                          (var, f.split('/meta-custom/')[-1], ln)))
                continue
            refs = re.findall(r'\$\{([A-Za-z0-9_]+)\}', literal)
            # Blame only a reference whose own value carries the placement:
            # an op that merely shares the variable with a hook is not it.
            carriers = [x for x in refs
                        if any(rx.search(img_final.get(x, '')) for _, rx in pats)]
            if not carriers:
                continue
            if all(x.startswith('MED_') or x in GENERIC_REFS for x in carriers):
                via_hook += 1
                continue
            v.append(('F6', '%s: %s:%d decides a value naming placement through %s' %
                      (var, f.split('/meta-custom/')[-1], ln,
                       ', '.join('${%s}' % x for x in carriers if not x.startswith('MED_')))))

    info.insert(0, 'graph: %d recipes, %d edges; %d stack->adjunct edges through hooks; '
                   '%d image values placed through hooks; collected in %s s' %
                (len(files), len(edges), hook_edges, via_hook, secs))
    return image, v, info


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == 'collect':
        collect(sys.argv[2], sys.argv[3])
        return 0
    if len(sys.argv) >= 2 and sys.argv[1] == 'analyze':
        profiles = sys.argv[2:] or sorted(d for d in os.listdir(OUT)
                                          if os.path.exists(os.path.join(OUT, d, 'profile.txt')))
        total = 0
        for p in profiles:
            image, v, info = analyze(p)
            print('[%s] %s: %s' % (p, image, 'ok' if not v else '%d violation(s)' % len(v)))
            for line in info:
                print('    %s' % line)
            for rule, msg in sorted(set(v)):
                print('%s [%s] %s' % (rule, p, msg))
            total += len(set(v))
        print('level B: %d profile(s), %d violation(s)' % (len(profiles), total))
        return 1 if total else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
