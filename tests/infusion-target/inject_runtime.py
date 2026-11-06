"""Runtime fault injection: the assertions whose fault can be injected into a
running device, with no image rebuilt - the journal seal's two, the
conformity plan's "seal seen to fail" (phase 2.2), and the four that phase 6
of implementation_plan_infusion_pump.md added.

For each one: run the assertion as the suite does and require a pass; inject
its fault; run it again and require a fail; undo the fault. Needs a FRESH disk
(the seal's assertions pass only on the provisioning boot, finding A8 of the
conformity plan): regenerate it with
    kas shell kas/project-infusion-qemu.yml -c "bitbake -C image_wic med-image-infusion"
(-C, not -f -c: the latter rewrites the .wic and leaves the deployed link on
the old one, so the boot finds the old disk).

Usage: python3 tests/infusion-target/inject_runtime.py
"""
import importlib.util
import os
import re
import shutil
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
spec = importlib.util.spec_from_file_location("medcheck", os.path.join(ROOT, "scripts", "med-check.py"))
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

CHECKS = {c[0]: c for c in mc.PLATFORM_CHECKS + mc.INFUSION_CHECKS}
results = []


def assertion(con, cid):
    _, _, command, predicate = CHECKS[cid]
    if callable(command):
        command = command()
    out = con.run(cid, command) or ""
    ok, detail = predicate(out)
    return ok, detail, out


def case(con, cid, inject, undo, label):
    before, detail0, _ = assertion(con, cid)
    if inject:
        inject()
    after, detail1, _ = assertion(con, cid)
    if undo:
        undo()
    good = before and not after
    results.append((cid, label, before, after, good, detail1))
    print("%-20s %-48s antes=%s depois=%s  %s\n    -> %s" % (
        cid, label, "PASS" if before else "FAIL", "PASS" if after else "FAIL",
        "OK" if good else "NAO VIU", detail1), flush=True)


def main():
    os.chdir(ROOT)
    con = mc.Console(["make", "runqemu-infusion"])
    try:
        if not con.expect(r"login:|Control-D to continue", mc.BOOT_TIMEOUT, "login"):
            # Twice a provisioning boot did not reach the prompt and left no
            # trace; the console is kept now, so the next one does.
            dump = os.path.join(ROOT, "build", "infusion-inject", "boot-timeout.log")
            os.makedirs(os.path.dirname(dump), exist_ok=True)
            with open(dump, "w", errors="replace") as f:
                f.write(con.buf)
            print("sem prompt; console em", dump)
            return 2
        for attempt in range(12):
            con.buf = ""
            con.send("root" if attempt == 0 else "")
            if con.expect(r"[#$] $|[#$]\s*$", 15, "shell"):
                break
        con.buf = ""
        con.send(f"dmesg -n 1 2>/dev/null; PS1='{mc.PROMPT}\\n'; export PS1")
        con.expect(re.escape(mc.PROMPT), 30, "prompt")
        con.run("settle", "sleep 25; systemctl is-system-running --wait")

        # The seal. journal-seal-key first: it collects the key the others use.
        ok, detail, _ = assertion(con, "journal-seal-key")
        print("journal-seal-key     (recolhimento)", ok, detail)
        key = mc.SEAL.get("key")
        path = os.path.join(mc.SEAL_KEYS_DIR, mc.SEAL.get("mid", "x") + ".key")

        # The conformity plan's phase 2.2: one byte of a sealed file that
        # verifies, changed in a copy, and verified again with the right key.
        # In short commands: BusyBox ash edits at most 1024 characters a line,
        # and one long command was cut there, left a quote open and hung the
        # console (the same reason med-check.py refuses commands over 900).
        # Shell variables carry from one command to the next.
        steps = [
            "M=$(cat /etc/machine-id); journalctl --rotate; sleep 1; F=; for f in "
            "/var/log/journal/$M/system@*.journal; do journalctl --header --file=$f | grep -q SEALED "
            "|| continue; journalctl --verify --verify-key=%s --file=$f >/dev/null 2>&1 && F=$f && break; "
            "done; echo file=$F" % key,
            "O=$(strings -t d $F | awk '$2 ~ /^MESSAGE=/ {print $1}' | sed -n 5p); P=$((O+8)); echo offset=$P",
            # BusyBox dd here has no conv=notrunc (it prints its usage and
            # changes nothing - one run read a PASS off an untouched copy), so
            # the copy is rebuilt around the one byte.
            # Nor has its head a -c: the first P bytes come from dd as one
            # block of P, which this dd does take.
            "C=/run/seal-copy.journal; dd if=$F of=$C bs=$P count=1 2>/dev/null; printf 'X' >> $C; "
            "tail -c +$((P+2)) $F >> $C; "
            "echo sizes=$(stat -c %s $F)/$(stat -c %s $C)",
            # One byte, proved cheaply: the first difference at P+1 (cmp
            # counts from 1) and everything after it identical.
            "echo first=$(cmp $F $C | sed 's/.*char \\([0-9]*\\).*/\\1/') "
            "rest=$(tail -c +$((P+2)) $F | md5sum | cut -c1-8)/$(tail -c +$((P+2)) $C | md5sum | cut -c1-8)",
            "journalctl --verify --verify-key=%s --file=$C 2>&1 | tail -n 2; "
            "echo original_rc=$(journalctl --verify --verify-key=%s --file=$F >/dev/null 2>&1; echo $?) "
            "copy_rc=$(journalctl --verify --verify-key=%s --file=$C >/dev/null 2>&1; echo $?)" % (key, key, key),
            "echo nokey_rc=$(journalctl --verify --file=$C >/dev/null 2>&1; echo $?); rm -f $C",
        ]
        for n, step in enumerate(steps):
            assert len(step) < 900, (n, len(step))
        out = "\n".join((con.run("byte%d" % n, step) or "") for n, step in enumerate(steps))
        pos = re.search(r"offset=(\d+)[\s\S]*sizes=(\d+)/(\d+)[\s\S]*first=(\d+) rest=(\w+)/(\w+)", out)
        same_size = bool(pos) and int(pos.group(4)) == int(pos.group(1)) + 1 and \
            pos.group(5) == pos.group(6) and pos.group(2) == pos.group(3)
        m = re.search(r"original_rc=(\d+) copy_rc=(\d+)", out)
        good = bool(m) and same_size and m.group(1) == "0" and m.group(2) != "0"
        results.append(("seal-byte", "um byte alterado num arquivo selado", m and m.group(1) == "0",
                        not (m and m.group(2) != "0"), good, out.strip().splitlines()[-2:] if out else ""))
        print("seal-byte            um byte alterado num arquivo selado: %s\n    %s" % (
            "OK" if good else "NAO VIU", out.strip().replace("\n", "\n    ")), flush=True)

        def wrong_key():
            mc.SEAL["key"] = mc.wrong_key(key)

        def right_key():
            mc.SEAL["key"] = key

        case(con, "journal-sealed", wrong_key, right_key, "a chave no host não é a do aparelho")

        def lose_key():
            shutil.move(path, path + ".aside")
            con.run("hide", "mv /run/med-journal-seal/verify-key /run/med-journal-seal/aside 2>/dev/null; true")

        def find_key():
            shutil.move(path + ".aside", path)
            con.run("unhide", "mv /run/med-journal-seal/aside /run/med-journal-seal/verify-key 2>/dev/null; true")

        case(con, "journal-seal-key", lose_key, find_key, "provisionamento sem testemunha")

        # The pump's phase 6 assertions.
        conf = "/etc/medplatform/infusion.conf"

        def alter_library():
            con.run("alter", "cp %s /run/conf.bak; sed -i 's/^drug.heparin.hard_max_ml_h = 50$/"
                    "drug.heparin.hard_max_ml_h = 500/' %s" % (conf, conf))

        def restore_library():
            out = con.run("restart", "systemctl restart infusion-control; sleep 3; "
                          "systemctl is-active infusion-control; journalctl -q -u infusion-control -n 8 "
                          "--no-pager -o cat | grep -c 'could not be loaded or verified'")
            print("    o serviço, reiniciado com a biblioteca alterada:", (out or "").split())
            con.run("restore", "cp /run/conf.bak %s; systemctl reset-failed infusion-control; "
                    "systemctl restart infusion-control; sleep 3" % conf)

        case(con, "drug-library-seal", alter_library, restore_library, "um limite alterado na biblioteca")

        def raise_alarm():
            con.run("alarm", "infusion-ctl program drug=saline syringe=50ml rate_ml_h=100 vtbi_ml=20; "
                    "infusion-ctl start; sleep 1; infusion-sim-fault air 1; sleep 1")

        def clear_alarm():
            con.run("clear", "infusion-sim-fault air 0; sleep 0.5; infusion-ctl acknowledge operator=x; "
                    "infusion-ctl stop; true")

        case(con, "infusion-idle", raise_alarm, clear_alarm, "o controlador em alarme")

        def no_mechanism():
            con.run("nosim", "systemctl stop infusion-sim; systemctl restart infusion-control --no-block; sleep 12")

        def mechanism_back():
            con.run("sim", "systemctl start infusion-sim; systemctl reset-failed infusion-control; "
                    "systemctl restart infusion-control; sleep 4")

        case(con, "infusion-active", no_mechanism, mechanism_back, "o simulador ausente")

        def kill_sim():
            con.run("killsim", "systemctl kill -s KILL infusion-sim; sleep 3")

        case(con, "infusion-sim-active", kill_sim, None, "o simulador morto e reiniciado")

        con.buf = ""
        con.send("poweroff")
        con.expect(r"Power down", 90, "poweroff")
    finally:
        con.close()

    seen = sum(1 for r in results if r[4])
    print("\n%d de %d injeções vistas" % (seen, len(results)))
    return 0 if seen == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
