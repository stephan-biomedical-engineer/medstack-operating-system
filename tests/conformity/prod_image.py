"""Phase 5.5 of docs/implementation_plan_conformity.md: verify the production
image without a shell on it.

A production image has nothing to log in to, so the runtime suite
(scripts/med-check.py) cannot run there. What it can be checked by is what
an inspector of a fielded device would have, in three layers:

  1. the console: the image boots to its application with no failed unit, and
     the console refuses root (E2: a failure would show by itself);
  2. power off from outside (an ACPI power button, QEMU's system_powerdown), so
     the trail is closed by the device itself and not by a test harness;
  3. the disk read after shutdown, from ANOTHER system: the development image
     boots, the production disk is attached to it read-only, its /data is
     opened with the key on its own key store, and its trail is read with
     journalctl -D and replayed with med-audit-verify. The acquisition
     service's SystemStart record is the proof that it ran, and an encrypted
     /data is the precondition for that record to exist (it requires /data's
     dm/uuid to start with CRYPT-).

Usage: python3 tests/conformity/prod_image.py
       (after make qemu IMAGE=med-image-eeg-prod and make qemu)
Reads the production disk image; never writes it (the second boot attaches it
with readonly=on).
"""
import importlib.util
import os
import re
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
spec = importlib.util.spec_from_file_location("medcheck", os.path.join(ROOT, "scripts", "med-check.py"))
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

PROD = "med-image-eeg-prod"
# The path as the kas container sees it: runqemu, and the QEMU monitor with it,
# runs inside the container.
PROD_WIC_IN_CONTAINER = "/work/build/tmp-glibc/deploy/images/qemux86-64/%s-qemux86-64.rootfs.wic" % PROD
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
RUN_SECONDS = 60


def monitor(con, command):
    """One command to the QEMU monitor, multiplexed on the serial (-serial mon:stdio).

    The monitor echoes a typed line one character at a time, redrawing it, so
    the answer is waited for (the next "(qemu)" prompt) rather than slept on.
    Returns the monitor's reply."""
    os.write(con.master, b"\x01c")
    con.expect(r"\(qemu\) ?$", 20, "monitor")
    con.buf = ""
    con.send(command)
    con.expect(r"\n[^\n]*\(qemu\) ?$", 60, "resposta do monitor")
    reply = ANSI.sub("", con.buf)
    os.write(con.master, b"\x01c")
    con.pump(2)
    return reply.splitlines()[-2] if "\n" in reply else reply


def boot_product():
    con = mc.Console(["make", "runqemu", "IMAGE=" + PROD])
    out = {}
    try:
        out["login prompt"] = con.expect(r"login:", mc.BOOT_TIMEOUT, "login")
        con.pump(RUN_SECONDS)
        con.buf = ""
        con.send("root")
        con.expect(r"[Pp]assword:", 15, "password")
        con.send("")
        out["root refused"] = con.expect(r"Login incorrect", 30, "recusa")
        out["no shell"] = not re.search(r"[#$] $", con.buf)
        con.buf = ""
        monitor(con, "system_powerdown")
        con.expect(r"reboot: Power down", 180, "desligamento")
    finally:
        con.close()
    text = ANSI.sub("", "".join(con.log))
    # Read from the whole log: Console.expect can stop at end of file in the
    # same read that brought the line it waits for.
    out["powered off"] = "reboot: Power down" in text.split("Login incorrect", 1)[-1]
    out["failed units"] = sorted(set(re.findall(r"\[FAILED\] (?:Failed to start )?(.*)", text)))
    out["acquisition started"] = "Started MedPlatform EEG acquisition service" in text
    out["dependency failures"] = sorted(set(re.findall(r"\[DEPEND\] (.*)", text)))
    return out, text


# Commands for the development guest. Each stays under the console's line
# limit (scripts/med-check.py: BusyBox ash hangs past 1024 characters).
FIND = ("for d in /sys/block/vd* /sys/block/sd*; do [ -e $d ] || continue; n=${d##*/}; "
        "grep -q \"^/dev/$n\" /proc/mounts || echo DISK=$n; done")
OPEN = ("D=/dev/{disk}; for p in $D[0-9]*; do L=$(blkid -o value -s PARTLABEL $p); "
        "[ \"$L\" = esp ] && E=$p; [ \"$L\" = med-data ] && V=$p; done; "
        "mkdir -p /run/pe /run/pd; mount -o ro -t vfat $E /run/pe && "
        "cryptsetup open --readonly --key-file /run/pe/medplatform/data.key $V prodata && "
        "mount -o ro,noload /dev/mapper/prodata /run/pd && echo OPENED=1; "
        "grep -c CRYPT- /sys/block/dm-*/dm/uuid | tail -n 1")
READ = ("J=/run/pd/state/journal; echo DIRS=$(ls $J | wc -l); "
        "echo BOOTS=$(journalctl -D $J --list-boots -q | wc -l); "
        "echo STARTS=$(journalctl -D $J -q MED_AUDIT=1 MED_COMPONENT=eeg-acquisition-service "
        "MED_AUDIT_EVENT=SYSTEM_START | wc -l); "
        "journalctl -D $J -o export MED_AUDIT=1 > /run/p.export; "
        "med-audit-verify /run/p.export > /run/p.out; echo VRC=$?; tail -n 1 /run/p.out; "
        "journalctl -D $J -q -o cat -u systemd-remount-fs.service | tail -n 4")
CLOSE = "umount /run/pd; cryptsetup close prodata; umount /run/pe; rm -f /run/p.export /run/p.out; echo CLOSED"


def read_disk():
    con = mc.Console(["make", "runqemu"])
    outs = []
    try:
        if not con.expect(r"login:|Control-D to continue", mc.BOOT_TIMEOUT, "login"):
            raise SystemExit("o sistema de desenvolvimento não deu prompt")
        for attempt in range(12):
            con.buf = ""
            con.send("root" if attempt == 0 else "")
            if con.expect(r"[#$] $|[#$]\s*$", 15, "shell"):
                break
        con.buf = ""
        con.send(f"dmesg -n 1 2>/dev/null; PS1='{mc.PROMPT}\\n'; export PS1")
        con.expect(re.escape(mc.PROMPT), 30, "prompt")
        con.run("settle", "sleep 20")
        before = con.run("before", FIND) or ""
        # As a USB stick: q35's root bus takes no PCI hotplug, its USB bus
        # does, and a disk taken out of a device and read on a bench is that.
        print("monitor:", monitor(con, "drive_add 0 if=none,id=prod,readonly=on,format=raw,file="
                                  + PROD_WIC_IN_CONTAINER))
        print("monitor:", monitor(con, "device_add usb-storage,drive=prod,id=proddisk,removable=on"))
        con.run("hotplug", "sleep 8")
        after = con.run("after", FIND) or ""
        new = sorted(set(re.findall(r"DISK=(\w+)", after)) - set(re.findall(r"DISK=(\w+)", before)))
        if len(new) != 1:
            raise SystemExit("disco de produção não apareceu (antes %r, depois %r)" % (before, after))
        outs.append(con.run("open", OPEN.format(disk=new[0])) or "")
        outs.append(con.run("read", READ) or "")
        outs.append(con.run("close", CLOSE) or "")
        con.buf = ""
        con.send("poweroff")
        con.expect(r"Power down", 90, "poweroff")
    finally:
        con.close()
        with open(os.path.join(ROOT, "build", "prod-reader-console.log"), "w") as f:
            f.write("".join(con.log))
    return "\n".join(outs)


def main():
    os.chdir(ROOT)
    product, console = boot_product()
    with open(os.path.join(ROOT, "build", "prod-console.log"), "w") as f:
        f.write(console)
    print("console:", {k: v for k, v in product.items()})

    disk = read_disk()
    print(disk.strip())
    kv = dict(re.findall(r"(\w+)=(\S+)", disk))
    report = re.search(r"entries=(\d+) components=(\d+) breaks=(\d+)", disk)

    checks = {
        "boota até o prompt": product["login prompt"],
        "nenhuma unidade falhou no boot": not product["failed units"] and not product["dependency failures"],
        "o serviço de aquisição subiu (console)": product["acquisition started"],
        "o console recusa root e não dá shell": product["root refused"] and product["no shell"],
        "desligou pelo botão de energia": product["powered off"],
        "o /data do produto abre com a chave do próprio disco": kv.get("OPENED") == "1",
        "a trilha do produto tem SystemStart do serviço de aquisição": int(kv.get("STARTS", 0)) >= 1,
        "a cadeia do produto verifica fora dele, sem quebra": bool(report) and report.group(3) == "0"
                                                              and kv.get("VRC") == "0",
    }
    for claim, ok in checks.items():
        print("%s  %s" % ("PASS" if ok else "FAIL", claim))
    if product["failed units"]:
        print("falharam:", product["failed units"])
    print("diretórios de journal (um por machine-id): %s; boots na trilha: %s" % (kv.get("DIRS"), kv.get("BOOTS")))
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
