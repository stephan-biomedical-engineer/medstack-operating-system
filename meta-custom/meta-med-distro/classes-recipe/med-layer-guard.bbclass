# The layer-boundary rules as part of building an image
# (docs/implementation_plan_layer_check.md, level C, path 2).
#
# scripts/med-layer-check.py runs over the MedPlatform layers before do_rootfs,
# and a violation fails the task: the image is not produced while a boundary is
# broken. make check-layers runs the same script outside BitBake, in seconds;
# this is what makes "verified during the build" literal rather than "verified
# by a target nobody has to run".
#
# When it runs. The task is signed by the CONTENT of every file in the
# MedPlatform layers and of the checker itself ([file-checksums], which walks
# directories), so it runs again whenever one of them changes and never
# otherwise. [nostamp] was rejected: BitBake reruns every task that depends on
# a nostamp task, so do_rootfs would have run on every build. The price of this
# design is the converse: a change that is only a comment in a layer file
# changes the task's signature and so reruns do_rootfs once.
#
# What it does not do: it is level A only, text. The effective dependency graph
# (level B) needs `bitbake -g` per profile and stays a separate target,
# make check-layers-graph.
#
# This class lives in the OS policy layer and names no machine, bootloader or
# device - it is held to the rules it enforces.

def med_layers_root(d):
    import os
    for layer in (d.getVar('BBLAYERS') or '').split():
        if os.path.basename(os.path.normpath(layer)) == 'meta-med-distro':
            return os.path.dirname(os.path.normpath(layer))
    return ''

def med_layer_dirs(d):
    import os
    root = d.getVar('MED_LAYERS_ROOT')
    if not root or not os.path.isdir(root):
        return []
    return sorted(os.path.join(root, n) for n in os.listdir(root)
                  if n.startswith('meta-med') and os.path.isdir(os.path.join(root, n)))

def med_layer_checksums(d):
    paths = med_layer_dirs(d) + [d.getVar('MED_LAYER_CHECK')]
    return ' '.join('%s:True' % p for p in paths if p)

# The MedPlatform layers sit side by side in one directory, and the checker in
# scripts/ beside it; both are derived from where meta-med-distro is, and both
# may be set explicitly where a checkout is laid out differently.
MED_LAYERS_ROOT ?= "${@med_layers_root(d)}"
MED_LAYER_CHECK ?= "${@os.path.join(os.path.dirname(d.getVar('MED_LAYERS_ROOT')), 'scripts', 'med-layer-check.py') if d.getVar('MED_LAYERS_ROOT') else ''}"

python do_med_layer_check() {
    import os, subprocess, sys

    root = d.getVar('MED_LAYERS_ROOT')
    script = d.getVar('MED_LAYER_CHECK')
    if not root or not os.path.isdir(root):
        bb.fatal("med-layer-guard: MED_LAYERS_ROOT (%r) is not the directory holding the "
                 "MedPlatform layers; the layer boundary cannot be checked" % root)
    if not script or not os.path.isfile(script):
        bb.fatal("med-layer-guard: the checker MED_LAYER_CHECK (%r) does not exist; an image "
                 "is not built without its layer boundary being checked" % script)

    # Machine names are learned from the other layers in this configuration.
    others = [l for l in (d.getVar('BBLAYERS') or '').split()
              if not os.path.normpath(l).startswith(os.path.normpath(root) + os.sep)]
    cmd = [sys.executable, script, '--root', root]
    for l in others:
        cmd += ['--layers-dir', l]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        bb.fatal("the MedPlatform layer boundary is violated - the image is not built.\n"
                 "%s\nThe same report, outside BitBake: make check-layers" % r.stdout.rstrip())
    for line in r.stdout.splitlines():
        if line.startswith(('summary:', 'exceptions:')):
            bb.note(line)
}
do_med_layer_check[file-checksums] = "${@med_layer_checksums(d)}"
do_med_layer_check[vardepsexclude] = "BBLAYERS"
addtask med_layer_check before do_rootfs
