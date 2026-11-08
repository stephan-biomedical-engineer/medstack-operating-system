"""ACE-03 on QEMU: service access to the production image is authenticated,
off by default, bounded in time and audited.

One boot of med-image-eeg-prod, driven only from outside - the QEMU monitor
plugs media in as USB sticks, and the SSH client runs in the kas container,
where runqemu's port forward (127.0.0.1:2222 -> guest 22) is:

  1. before any grant, nothing answers SSH;
  2. a grant signed by a key the device does not trust opens nothing;
  3. the real grant opens a listener; the service user logs in with the
     granted key, reads and verifies the trail; root, and the service user
     with another key, are refused;
  4. when the grant's duration runs out, the listener is gone and the session
     with it;
  5. the same grant plugged in again opens nothing (one window per grant);
  6. after power-off, the product's disk read from a development guest
     (tests/conformity/prod_image.py) holds every step as an audit record of
     med-service-access, and the chain verifies.

Usage: python3 tests/conformity/service_access.py
       (after make qemu IMAGE=med-image-eeg-prod and make qemu)
"""
import importlib.util
import os
import re
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mc = load("medcheck", "scripts/med-check.py")
pi = load("prodimage", "tests/conformity/prod_image.py")

WORK = os.path.join(ROOT, "build", "service-test")
IN_CONTAINER = "/work/build/service-test"
DURATION = 150


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def prepare():
    sh("rm -rf %s && mkdir -p %s" % (WORK, WORK))
    for k in ("tech", "other"):
        sh("ssh-keygen -q -t ed25519 -N '' -C %s@bench -f %s/%s" % (k, WORK, k))
    r = sh("scripts/med-service-grant.sh %s/tech.pub %d 'acceptance test'" % (WORK, DURATION))
    if r.returncode != 0:
        raise SystemExit("med-service-grant.sh: " + r.stderr)
    sh("mv build/service-grant.img %s/good.img" % WORK)
    # The forged one: the same kind of text, signed by a code-signing
    # certificate the device has never seen.
    sh("cd %s && openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout f.key "
       "-out f.pem -subj /CN=forger -days 30 -addext extendedKeyUsage=critical,codeSigning >/dev/null 2>&1" % WORK)
    compat = re.search(r"grant for (\S+)", r.stdout).group(1)
    key = " ".join(open(WORK + "/tech.pub").read().split()[:3])
    text = ("med-service-grant 1\ncompatible %s\nduration %d\nnonce %s\nkey %s\nreason forged\n"
            % (compat, DURATION, "f" * 32, key))
    open(WORK + "/forged.txt", "w").write(text)
    sh("cd %s && mkdir -p ff && openssl cms -sign -binary -nodetach -outform DER -md sha256 -signer f.pem "
       "-inkey f.key -in forged.txt -out ff/grant.p7s && truncate -s 8M forged.img && "
       "mkfs.ext4 -F -q -L med-service -d ff forged.img" % WORK)
    os.chmod(WORK + "/tech", 0o600)
    os.chmod(WORK + "/other", 0o600)


def container():
    return sh("docker ps -q --filter ancestor=%s" % mc.KAS_IMAGE).stdout.split()[0]


def banner():
    """The first bytes that answer on the forwarded SSH port, from inside the container."""
    probe = ("import socket;s=socket.create_connection(('127.0.0.1',2222),5);s.settimeout(5)\n"
             "try:\n print(s.recv(16).decode(errors='replace'))\nexcept Exception as e:\n print('')")
    r = sh("docker exec %s python3 -c \"%s\"" % (container(), probe))
    return r.stdout.strip()


def ssh(user, key, command):
    r = sh("docker exec %s ssh -p 2222 -i %s/%s -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
           "-o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR %s@127.0.0.1 '%s'"
           % (container(), IN_CONTAINER, key, user, command))
    return r.returncode, (r.stdout + r.stderr).strip()


def plug(con, name, image):
    pi.monitor(con, "drive_add 0 if=none,id=%s,readonly=on,format=raw,file=%s/%s" % (name, IN_CONTAINER, image))
    reply = pi.monitor(con, "device_add usb-storage,drive=%s,id=%s,removable=on" % (name, name))
    con.pump(15)
    return reply


def unplug(con, name):
    pi.monitor(con, "device_del %s" % name)
    con.pump(5)


READ = ("J=/run/pd/state/journal; for a in refused granted session_start session_end closed; do "
        "echo $a=$(journalctl -D $J -q MED_COMPONENT=med-service-access MED_ACCESS_ACTION=$a | wc -l); done; "
        "journalctl -D $J -o export MED_COMPONENT=med-service-access MED_ACCESS_ACTION=closed "
        "| grep -a '^MED_ACCESS_REASON=' | tail -n 2; "
        "journalctl -D $J -o export MED_AUDIT=1 > /run/p.export; "
        "med-audit-verify /run/p.export > /run/p.out; echo VRC=$?; grep -c med-service-access /run/p.out; "
        "tail -n 1 /run/p.out")


def main():
    os.chdir(ROOT)
    prepare()
    res = {}
    con = mc.Console(["make", "runqemu", "IMAGE=" + pi.PROD])
    try:
        if not con.expect(r"login:", mc.BOOT_TIMEOUT, "login"):
            raise SystemExit("o produto não deu prompt")
        con.pump(30)
        res["closed before any grant"] = not banner().startswith("SSH-")

        plug(con, "forged", "forged.img")
        res["forged grant opens nothing"] = not banner().startswith("SSH-")
        unplug(con, "forged")

        plug(con, "good", "good.img")
        res["grant opens a listener"] = banner().startswith("SSH-")
        rc, out = ssh("med-service", "tech", "id -un; med-trail-export 2>&1 >/dev/null | tail -n 1")
        print("sessão de serviço:", rc, out)
        res["service user logs in with the granted key"] = rc == 0 and "med-service" in out
        res["the trail exports and verifies on the device"] = "breaks=0" in out
        rc, out = ssh("root", "tech", "id")
        res["root refused"] = rc != 0
        rc, out = ssh("med-service", "other", "id")
        res["another key refused"] = rc != 0
        rc, out = ssh("med-service", "tech", "touch /etc/x 2>&1; echo logged-in; exit 0")
        # Logged in, and the write refused by the filesystem. "denied" alone
        # would also match a login that failed ("Permission denied
        # (publickey)"), which the injection without the sshd mask showed.
        res["the service user cannot write the rootfs"] = (rc == 0 and "logged-in" in out
                                                            and "Read-only file system" in out)
        unplug(con, "good")

        time.sleep(DURATION + 15)
        res["the window closes when the duration ends"] = not banner().startswith("SSH-")

        plug(con, "again", "good.img")
        res["the same grant opens nothing twice"] = not banner().startswith("SSH-")

        con.buf = ""
        pi.monitor(con, "system_powerdown")
        con.expect(r"reboot: Power down", 180, "desligamento")
    finally:
        con.close()
        with open(os.path.join(ROOT, "build", "service-access-console.log"), "w") as f:
            f.write("".join(con.log))

    original = pi.READ
    pi.READ = READ
    try:
        disk = pi.read_disk()
    finally:
        pi.READ = original
    print(disk.strip())
    kv = dict(re.findall(r"^(\w+)=(\S+)", disk, re.M))
    report = re.search(r"entries=(\d+) components=(\d+) breaks=(\d+)", disk)
    res["trail: refusals recorded (forged, replay)"] = int(kv.get("refused", 0)) >= 2
    res["trail: one grant"] = kv.get("granted") == "1"
    res["trail: every connection started and ended"] = (int(kv.get("session_start", 0)) >= 4
                                                         and kv.get("session_start") == kv.get("session_end"))
    res["trail: window closed, expired"] = int(kv.get("closed", 0)) >= 1 and "MED_ACCESS_REASON=expired" in disk
    res["trail: chain verifies off the device"] = bool(report) and report.group(3) == "0" and kv.get("VRC") == "0"

    for claim, ok in res.items():
        print("%s  %s" % ("PASS" if ok else "FAIL", claim))
    return 0 if all(res.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
