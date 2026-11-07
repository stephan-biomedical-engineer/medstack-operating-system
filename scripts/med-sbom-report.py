#!/usr/bin/env python3
"""Count what an image's SBOM says, and what it does not.

  python3 scripts/med-sbom-report.py [IMAGE]      (default med-image-eeg)

Reads the SPDX 2.2 archive the build deploys for the image
(INHERIT += "create-spdx", med-os.conf) and reports, over the packages
installed in it: how many declare a licence, a version and a supplier; how
many name where their source came from; and, over the recipes behind them,
how many carry a CPE (what a vulnerability database is matched against) and
how many list CVEs already fixed by a patch. Implementation plan of
conformity, phase 4 (INV-01, INV-02).

It reads only; it decides nothing about any licence or vulnerability.
"""
import collections
import glob
import io
import json
import os
import re
import sys
import tarfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEPLOY = os.path.join(ROOT, "build", "tmp-glibc", "deploy", "images", "qemux86-64")
EMPTY = ("", "NOASSERTION", "NONE", None)


def own_recipes():
    names = set()
    for path in glob.glob(os.path.join(ROOT, "meta-custom", "*", "recipes-*", "*", "*.bb")):
        names.add(re.sub(r"_[^_]*$", "", os.path.basename(path)[:-3]))
    return names


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "med-image-eeg"
    archive = os.path.realpath(os.path.join(DEPLOY, image + "-qemux86-64.rootfs.spdx.tar.zst"))
    import subprocess
    raw = subprocess.run(["zstd", "-dc", archive], capture_output=True, check=True).stdout
    docs = {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        for m in tar.getmembers():
            if m.name.endswith(".spdx.json"):
                docs[os.path.basename(m.name)] = json.load(tar.extractfile(m))

    installed = os.path.join(ROOT, "build", "buildhistory", "images", "qemux86_64", "glibc", image,
                             "installed-package-names.txt")
    names = [l.split()[0] for l in open(installed) if l.strip()]
    mine = own_recipes()

    count = collections.Counter()
    licences = collections.Counter()
    recipes = {}
    missing = []
    for name in names:
        doc = docs.get(name + ".spdx.json")
        if doc is None:
            missing.append(name)
            continue
        pkg = next((p for p in doc.get("packages", []) if p.get("SPDXID", "").startswith("SPDXRef-Package")), None)
        if pkg is None:
            missing.append(name)
            continue
        count["packages"] += 1
        lic = pkg.get("licenseDeclared")
        if lic not in EMPTY:
            count["licence"] += 1
            for token in re.findall(r"[A-Za-z0-9.+-]+(?:-only|-or-later)?", re.sub(r"DocumentRef-[^:]+:", "", lic)):
                if token not in ("AND", "OR", "WITH"):
                    licences[token.split("LicenseRef-")[-1]] += 1
        if pkg.get("versionInfo") not in EMPTY:
            count["version"] += 1
        if pkg.get("supplier") not in EMPTY:
            count["supplier"] += 1
        if pkg.get("downloadLocation") not in EMPTY:
            count["origin"] += 1
        m = re.search(r"DocumentRef-recipe-([A-Za-z0-9._+-]+)", json.dumps(doc))
        if m:
            recipes[m.group(1)] = True

    count["recipes"] = len(recipes)
    for recipe in recipes:
        doc = docs.get("recipe-%s.spdx.json" % recipe)
        if not doc:
            continue
        rec = doc["packages"][0]
        refs = rec.get("externalRefs", [])
        if any("cpe" in r.get("referenceType", "") for r in refs):
            count["cpe"] += 1
        if rec.get("downloadLocation") not in EMPTY:
            count["recipe-origin"] += 1
        if "CVEs fixed" in (rec.get("sourceInfo") or ""):
            count["cve-fixed"] += 1
            count["cves"] += len(re.findall(r"CVE-\d{4}-\d+", rec.get("sourceInfo", "")))
        if recipe in mine:
            count["own"] += 1

    n = count["packages"]
    r = count["recipes"]
    print("imagem %s: %d pacotes instalados, %d com documento SPDX, %d sem" % (image, len(names), n, len(missing)))
    print("  licença declarada   %d de %d" % (count["licence"], n))
    print("  versão              %d de %d" % (count["version"], n))
    print("  fornecedor          %d de %d" % (count["supplier"], n))
    print("  origem da fonte     %d de %d (downloadLocation)" % (count["origin"], n))
    print("receitas por trás deles: %d (%d deste repositório)" % (r, count["own"]))
    print("  com CPE             %d de %d" % (count["cpe"], r))
    print("  origem da fonte     %d de %d" % (count["recipe-origin"], r))
    print("  com CVEs corrigidos por patch %d de %d (%d CVEs)" % (count["cve-fixed"], r, count["cves"]))
    print("licenças mais comuns: %s" % ", ".join("%s %d" % kv for kv in licences.most_common(8)))
    if missing:
        print("sem documento: %s" % ", ".join(missing[:10]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
