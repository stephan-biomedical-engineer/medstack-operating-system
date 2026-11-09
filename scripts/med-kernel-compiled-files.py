#!/usr/bin/env python3
"""The kernel source files a build actually compiled, from kbuild's own records.

  python3 scripts/med-kernel-compiled-files.py BUILD_DIR OUT.json.zst

BUILD_DIR is the kernel recipe's build tree (linux-<machine>-standard-build
under the recipe's WORKDIR). Every object kbuild compiles leaves a .<obj>.cmd
file naming its source and every header the preprocessor opened; this reads
all of them and writes the union, as paths relative to the kernel source tree,
in the format of a Yocto debug-sources file
(pkgdata/<machine>/debugsources/<pn>-debugsources.json.zstd), which is what
poky's scripts/contrib/improve_kernel_cve_report.py takes with
--debug-sources-file.

Why not the real debug-sources file: poky derives it from DWARF, and the
QEMU kernel is built without CONFIG_DEBUG_INFO, so every entry in it is
empty (measured 2026-11-08: 482 entries, 0 files). Building the kernel with
debug information would give the list for a different kernel than the one
measured; kbuild's records are of this one. docs/RESULTS.md section 12.

Reads only. Exit 0 with a count on stdout; 1 when nothing was found.
"""
import json
import os
import re
import subprocess
import sys

PREFIX = re.compile(r"(/[^\s:]*?/kernel-source)/")
PATH = re.compile(r"(?:^|\s)(/[^\s\\]+\.(?:c|h|S))\b")


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    build, out = sys.argv[1], sys.argv[2]
    files = set()
    cmds = 0
    for root, _dirs, names in os.walk(build):
        for name in names:
            if not (name.startswith(".") and name.endswith(".o.cmd")):
                continue
            cmds += 1
            with open(os.path.join(root, name), errors="replace") as f:
                text = f.read()
            for path in PATH.findall(text):
                m = PREFIX.match(path)
                if m:
                    files.add(path[len(m.group(1)) + 1:])
    if not files:
        print("nenhum fonte encontrado em %s" % build, file=sys.stderr)
        return 1
    # One key, one list: improve_kernel_cve_report.py drops the first path
    # component of every entry ("kernel-source/<path>" -> "<path>").
    doc = json.dumps({"vmlinux-and-modules": sorted("kernel-source/" + p for p in files)})
    subprocess.run(["zstd", "-q", "-f", "-o", out], input=doc.encode(), check=True)
    c = sum(1 for p in files if p.endswith(".c"))
    print("%d arquivos .cmd; %d fontes compilados (%d .c, %d .h, %d .S) -> %s"
          % (cmds, len(files), c, sum(1 for p in files if p.endswith(".h")),
             sum(1 for p in files if p.endswith(".S")), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
