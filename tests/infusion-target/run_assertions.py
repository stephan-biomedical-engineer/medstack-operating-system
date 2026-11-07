"""Boot an image and run only the named assertions of scripts/med-check.py.

Usage: python3 tests/infusion-target/run_assertions.py RUNQEMU-TARGET ID [ID ...]
  e.g. run_assertions.py runqemu journal-persistent state-persistent
Exit 0 if every named assertion passed. For a quick look at a few assertions
without the whole suite - and for seeing one fail on an image that lacks
what it checks."""
import importlib.util
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
spec = importlib.util.spec_from_file_location("medcheck", os.path.join(ROOT, "scripts", "med-check.py"))
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)


def main():
    target, ids = sys.argv[1], sys.argv[2:]
    checks = {c[0]: c for name in dir(mc) if name.endswith("_CHECKS") for c in getattr(mc, name)}
    os.chdir(ROOT)
    con = mc.Console(["make", target])
    ok_all = True
    try:
        if not con.expect(r"login:|Control-D to continue", mc.BOOT_TIMEOUT, "login"):
            print("sem prompt")
            return 2
        for attempt in range(12):
            con.buf = ""
            con.send("root" if attempt == 0 else "")
            if con.expect(r"[#$] $|[#$]\s*$", 15, "shell"):
                break
        con.buf = ""
        con.send(f"dmesg -n 1 2>/dev/null; PS1='{mc.PROMPT}\\n'; export PS1")
        con.expect(re.escape(mc.PROMPT), 30, "prompt")
        con.run("settle", "sleep 20")
        for cid in ids:
            _, claim, command, predicate = checks[cid]
            out = con.run(cid, command() if callable(command) else command) or ""
            ok, detail = predicate(out)
            ok_all = ok_all and ok
            print("%s  %-22s %s\n    -> %s" % ("PASS" if ok else "FAIL", cid, claim, detail), flush=True)
        con.buf = ""
        con.send("poweroff")
        con.expect(r"Power down", 90, "poweroff")
    finally:
        con.close()
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
