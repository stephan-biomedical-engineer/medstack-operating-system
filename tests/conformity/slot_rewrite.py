"""AUD-06 on QEMU: does the audit trail outlive a rewrite of the rootfs slot?

QEMU runs with MED_BOOTLOADER = "noop", so a `rauc install` there writes the
inactive slot and never boots it (RESULTS.md section 6). What an update does
to the trail is, though, a property of the DISK: the slot the device runs from
is replaced by a fresh rootfs. So this does exactly that, between two boots:

  1. boot, note the machine-id, the audit records and the chain's head;
     power off;
  2. on the host, overwrite the med-root-a partition of the disk image with
     the build's fresh rootfs (.ext4) - an update of the running slot;
  3. boot again and check: the records from before are still in the journal,
     the new records continue the chain where it stopped, and the verifier
     finds no break across the rewrite.

The board's real slot switch is measured elsewhere (implementation_plan_uboot_ab.md
section 8); this is the QEMU-side evidence for the trail, and it says so.

Usage: python3 tests/conformity/slot_rewrite.py   (after make qemu)
Destructive for the deployed EEG disk image's slot A, which is the point;
rebuild the image (make qemu) to get a pristine one.
"""
import importlib.util
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
spec = importlib.util.spec_from_file_location("medcheck", os.path.join(ROOT, "scripts", "med-check.py"))
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

DEPLOY = os.path.join(ROOT, "build", "tmp-glibc", "deploy", "images", "qemux86-64")
WIC = os.path.join(DEPLOY, "med-image-eeg-qemux86-64.rootfs.wic")
EXT4 = os.path.join(DEPLOY, "med-image-eeg-qemux86-64.rootfs.ext4")

STATE = ("echo mid=$(cat /etc/machine-id); "
         "echo records=$(journalctl -m -q MED_AUDIT=1 MED_COMPONENT=eeg-acquisition-service | wc -l); "
         "echo head=$(cat /var/lib/medplatform/eeg-audit-chain.state); "
         "echo dirs=$(ls /var/log/journal | wc -l)")
VERIFY = ("journalctl -m -o export MED_AUDIT=1 MED_COMPONENT=eeg-acquisition-service > /run/a.export; "
          "med-audit-verify /run/a.export | tail -n 2; echo rc=$?; rm -f /run/a.export")


def boot_and(commands):
    con = mc.Console(["make", "runqemu"])
    outs = []
    try:
        if not con.expect(r"login:|Control-D to continue", mc.BOOT_TIMEOUT, "login"):
            raise SystemExit("sem prompt")
        for attempt in range(12):
            con.buf = ""
            con.send("root" if attempt == 0 else "")
            if con.expect(r"[#$] $|[#$]\s*$", 15, "shell"):
                break
        con.buf = ""
        con.send(f"dmesg -n 1 2>/dev/null; PS1='{mc.PROMPT}\\n'; export PS1")
        con.expect(re.escape(mc.PROMPT), 30, "prompt")
        con.run("settle", "sleep 25")
        for n, c in enumerate(commands):
            outs.append(con.run("c%d" % n, c) or "")
        con.buf = ""
        con.send("poweroff")
        con.expect(r"Power down", 90, "poweroff")
    finally:
        con.close()
    return outs


def kv(text):
    return dict(re.findall(r"(\w+)=(\S+)", text))


def slot_a():
    """Start and size, in 512-byte sectors, of med-root-a in the disk image."""
    dump = subprocess.run(["sfdisk", "-d", os.path.realpath(WIC)], capture_output=True, text=True,
                          check=True).stdout
    for line in dump.splitlines():
        if 'name="med-root-a"' in line:
            return int(re.search(r"start=\s*(\d+)", line).group(1)), int(re.search(r"size=\s*(\d+)", line).group(1))
    raise SystemExit("med-root-a não encontrado")


def main():
    os.chdir(ROOT)
    before = kv(boot_and([STATE])[0])
    print("antes  :", before)

    start, size = slot_a()
    ext4 = os.path.realpath(EXT4)
    if os.path.getsize(ext4) > size * 512:
        raise SystemExit("o rootfs não cabe no slot")
    # The update: the running slot replaced by a fresh rootfs, in place.
    subprocess.run(["dd", "if=" + ext4, "of=" + os.path.realpath(WIC), "bs=512", "seek=%d" % start,
                    "conv=notrunc,fsync", "status=none"], check=True)
    print("slot A regravado com %s (%d bytes) no setor %d" % (os.path.basename(ext4), os.path.getsize(ext4), start))

    state, verify = boot_and([STATE, VERIFY])
    after = kv(state)
    print("depois :", after)
    print(verify.strip())

    seq_before = int(before["head"].split()[0]) if before.get("head") else 0
    report = re.search(r"entries=(\d+) components=(\d+) breaks=(\d+)", verify)
    restarts = re.search(r"restarts=(\d+)", verify)
    checks = {
        "machine-id novo (o slot foi mesmo regravado)": after.get("mid") != before.get("mid"),
        "dois diretórios de journal em /data (o de antes e o novo)": int(after.get("dirs", 0)) >= 2,
        "os registros de antes continuam": int(after.get("records", 0)) > int(before.get("records", 0)),
        "a cadeia continuou: cabeça adiante da de antes": int(after.get("head", "0").split()[0] if after.get("head") else 0) > seq_before,
        "nenhuma quebra na cadeia através da regravação": bool(report) and report.group(3) == "0",
        "nenhum reinício da cadeia": bool(restarts) and restarts.group(1) == "0",
    }
    for claim, ok in checks.items():
        print("%s  %s" % ("PASS" if ok else "FAIL", claim))
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
