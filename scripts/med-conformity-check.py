#!/usr/bin/env python3
"""Check docs/CONFORMIDADE.md against the rules phase 1 of
docs/implementation_plan_conformity.md sets for it, and print its counts.

  1. Every technical requirement (RT) cites at least one clause of a standard
     that was checked (IEC 62304, ISO 14971, IEC 81001-5-1), and none still
     carries an unverified marker `[...?]`.
  2. Every clause of the normative matrix names at least one RT, or is
     category E (outside the software: a process of the manufacturer); every
     RT it names exists; and every RT is named by at least one clause - the
     traceability runs both ways.
  3. A clause's category is the one its RTs give it, never chosen by hand:
     all met with evidence that fails by itself (E2/E3) -> A; all met on
     inspection (E1) -> B; all open -> D; any other mix -> C.
  4. Every piece of evidence below the "nothing ran" levels points at
     something that exists: an assertion of scripts/med-check.py, a section
     of docs/RESULTS.md, or a file of this repository.

Exit 0 when the document holds, 1 when it does not. Python 3, no build.
"""
import importlib.util
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DOC = os.path.join(ROOT, "docs", "CONFORMIDADE.md")
CHECKED = ("62304", "14971", "81001-5-1")
SKIP_DIRS = {"build", "layers", ".git", "downloads", "sstate-cache", "linux-med", ".kas", "__pycache__"}


def table_rows(text, first_cell):
    rows = []
    for line in text.splitlines():
        if re.match(r"^\| %s \|" % first_cell, line):
            rows.append([c.strip() for c in line.strip().strip("|").split("|")])
    return rows


def assertions():
    spec = importlib.util.spec_from_file_location("medcheck", os.path.join(ROOT, "scripts", "med-check.py"))
    mc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mc)
    found = set()
    for name in dir(mc):
        if name.endswith("_CHECKS"):
            found |= {c[0] for c in getattr(mc, name)}
    return found


def repository_files():
    names = set()
    for root, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        rel = os.path.relpath(root, ROOT)
        for f in files:
            names.add(f)
            names.add(os.path.normpath(os.path.join(rel, f)))
        for d in dirs:
            names.add(d)
            names.add(os.path.normpath(os.path.join(rel, d)))
    return names


def status_of(cell):
    return cell.split()[0] if cell else "?"


def main():
    text = open(DOC).read()
    failures = []

    rts = {}
    for c in table_rows(text, r"[A-Z]{3}-\d\d"):
        if len(c) != 8:
            failures.append("%s: %d colunas, esperadas 8" % (c[0], len(c)))
            continue
        rts[c[0]] = {"origin": c[2], "evidence": c[4], "level": c[5], "status": status_of(c[7])}

    # 1. clauses cited, none unverified
    for rt, r in rts.items():
        if "?]" in r["origin"]:
            failures.append("%s: origem com marcador não conferido" % rt)
        if not any(n in r["origin"] for n in CHECKED):
            failures.append("%s: não cita cláusula de norma conferida" % rt)
    if "?]" in text:
        failures.append("o documento ainda tem marcadores `[...?]`")

    # 2 and 3. the matrix
    named = set()
    counts = {}
    matrix = table_rows(text, r"(62304|14971|81001-5-1)")
    for c in matrix:
        if len(c) != 6:
            failures.append("matriz, %s %s: %d colunas, esperadas 6" % (c[0], c[1], len(c)))
            continue
        norm, clause, _, ids_cell, cat, _ = c
        ids = re.findall(r"[A-Z]{3}-\d\d", ids_cell)
        for i in ids:
            if i not in rts:
                failures.append("matriz, %s %s: RT %s não existe" % (norm, clause, i))
        named |= set(ids)
        if not ids:
            if cat != "E":
                failures.append("matriz, %s %s: sem RT e categoria %s (só E pode ficar sem)" % (norm, clause, cat))
            expected = "E"
        else:
            states = [rts[i]["status"] for i in ids if i in rts]
            levels = [rts[i]["level"] for i in ids if i in rts]
            if all(s == "✅" for s in states):
                expected = "A" if any(l in ("E2", "E3") for l in levels) else "B"
            elif all(s == "✗" for s in states):
                expected = "D"
            else:
                expected = "C"
            if cat != expected:
                failures.append("matriz, %s %s: categoria %s, os RTs dão %s" % (norm, clause, cat, expected))
        counts[expected] = counts.get(expected, 0) + 1
    for rt in rts:
        if rt not in named:
            failures.append("%s: nenhuma cláusula da matriz o nomeia" % rt)

    # 4. evidence points at something that exists
    checks = assertions()
    files = repository_files()
    results = open(os.path.join(ROOT, "docs", "RESULTS.md")).read()
    sections = set(re.findall(r"^## (\d+)\.", results, re.M))
    for rt, r in rts.items():
        pointers = 0
        for token in re.findall(r"`([^`]+)`", r["evidence"]):
            if token in checks or token in files or os.path.basename(token) in files:
                pointers += 1
        for sec in re.findall(r"RESULTS\.md` §(\d+)", r["evidence"]):
            if sec in sections:
                pointers += 1
            else:
                failures.append("%s: RESULTS.md §%s não existe" % (rt, sec))
        if r["level"] not in ("—", "E0") and pointers == 0:
            failures.append("%s: nível %s sem nenhum ponteiro de evidência que exista" % (rt, r["level"]))

    by_status = {}
    by_level = {}
    for r in rts.values():
        by_status[r["status"]] = by_status.get(r["status"], 0) + 1
        by_level[r["level"]] = by_level.get(r["level"], 0) + 1
    print("RTs: %d  %s" % (len(rts), "  ".join("%s %d" % kv for kv in sorted(by_status.items()))))
    print("níveis: %s" % "  ".join("%s %d" % kv for kv in sorted(by_level.items())))
    print("matriz: %d cláusulas  %s" % (len(matrix), "  ".join("%s %d" % kv for kv in sorted(counts.items()))))
    for f in failures:
        print("FALHA  " + f)
    print("%d falha(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
