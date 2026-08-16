#!/usr/bin/env python3
"""MedPlatform runtime acceptance suite.

Boots the QEMU image, runs a fixed list of assertions against the running
system, and exits non-zero if any of them fail.

WHY THIS EXISTS
Everything this repository claims about its runtime behaviour was, until now,
verified by hand once and written down. IEC 62304 asks that verification be a
repeatable activity with recorded results, and "I ran it once and took notes"
is not that. It also matters for a more ordinary reason: of the six defects
found while implementing the A/B update path, five failed no build at all.
They produced artefacts that compiled, booted and ran. The only thing that
would have caught them earlier is executing the artefact and asserting on it.

HOW IT TALKS TO THE GUEST
Over the serial console, through a pty, because that is the one channel that
exists on every profile - no ssh, no network, no shared filesystem. Two things
make that reliable rather than flaky:

  - the kernel audit subsystem floods the console, so the first thing the suite
    does after logging in is "dmesg -n 1"; without it, kernel messages interleave
    with command output and the parsing becomes a guessing game;
  - each check is bracketed by unique markers and the prompt is replaced with a
    marker of its own, so nothing depends on matching a shell prompt that the
    guest is free to change.

Usage:  make check              the EEG device profile (all assertions)
        make check-tomograph    the tomograph profile (platform assertions only)
"""

import os
import pty
import re
import select
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = "@@MEDCHECK-READY@@"
KAS_IMAGE = "ghcr.io/siemens/kas/kas:5.2"

BOOT_TIMEOUT = 420
CMD_TIMEOUT = 90

# Units allowed to be failed. Every entry needs a reason, and the reason has to
# be a deliberate limitation rather than an unexplained failure - an allowlist
# nobody justifies is just a muted alarm.
EXPECTED_FAILED = {
    # nographic: there is no DRM/KMS device for a compositor to open. The Qt
    # HMI needs a graphical runqemu, which is a separate exercise.
    "weston.service",
}


def check_failed_units(out):
    got = {line.split()[0] for line in out.splitlines() if line.strip()}
    unexpected = got - EXPECTED_FAILED
    if unexpected:
        return False, "unidades falhas não previstas: " + ", ".join(sorted(unexpected))
    return True, "nenhuma além das previstas" if got else "nenhuma"


def check_exposure(out):
    m = re.search(r"exposure level for .*?: ([0-9.]+)", out)
    if not m:
        return False, "não consegui ler o exposure level"
    value = float(m.group(1))
    # A ratchet, not a target: this is the measured value at the time the suite
    # was written. It exists to catch a unit file that silently loses its
    # sandboxing, not to claim 4.0 is good.
    return value <= 4.0, f"exposure {value} (limite 4.0)"


def rx(pattern, flags=0):
    compiled = re.compile(pattern, flags)

    def predicate(out):
        return bool(compiled.search(out)), out.strip().splitlines()[-1] if out.strip() else "(vazio)"

    return predicate


# As asserções são divididas do mesmo jeito que a plataforma é. As de PLATFORM
# valem para qualquer imagem MedOS - são exatamente o que o perfil tomograph
# herda sem escrever uma linha - e as de EEG pertencem ao perfil de dispositivo.
#
# Rodar as primeiras contra o tomógrafo é o que transforma "a plataforma se
# sustenta sozinha, sem aplicação nenhuma" de inferência sobre listas de pacotes
# em observação sobre um sistema em execução.
PLATFORM_CHECKS = [
    ("system-state", "o sistema atingiu um estado terminal",
     "systemctl is-system-running || true", rx(r"^(running|degraded)$", re.M)),

    ("failed-units", "nenhuma unidade falha além das previstas",
     "systemctl list-units --state=failed --no-legend --plain | awk '{print $1}'",
     check_failed_units),

    ("journal-persistent", "/var/log/journal é diretório real, não tmpfs",
     "test -d /var/log/journal && test ! -L /var/log && echo OK", rx(r"OK")),

    ("journal-sealed", "o Forward Secure Sealing do journal valida",
     "journalctl --verify 2>&1 | tail -1", rx(r"^PASS", re.M)),

    # rauc.service is Type=dbus with BusName=de.pengutronix.rauc, so it is
    # activated on demand and being inactive is not a fault - on a platform
    # image with no application, nothing has asked it for anything yet. What
    # the platform owes is a service that is available and has not failed; that
    # its slots resolve is asserted separately, and does not need the daemon.
    ("rauc-available", "o serviço de atualização está disponível e não falhou",
     "systemctl is-failed rauc.service || true", rx(r"^(inactive|active)$", re.M)),

    ("rauc-slots", "os dois slots A/B resolvem e o bootado é identificado",
     "rauc status 2>/dev/null",
     rx(r"med-root-a[\s\S]*booted|booted[\s\S]*med-root-a")),

    ("rauc-booted-partition", "o rootfs veio de uma partição, não do disco inteiro",
     "rauc status 2>/dev/null | grep '^Booted from'", rx(r"/dev/vda2")),

    ("partlabels", "as quatro partições GPT existem por rótulo",
     "ls /dev/disk/by-partlabel/",
     rx(r"esp[\s\S]*med-data[\s\S]*med-root-a[\s\S]*med-root-b")),

    ("data-provisioned", "o volume /data foi provisionado e aberto",
     "systemctl is-active med-data-provision.service", rx(r"^active$", re.M)),

    ("data-mounted", "/data está montado a partir do mapeamento dm-crypt",
     "awk '$2==\"/data\"{print $1, $3}' /proc/mounts",
     rx(r"^/dev/mapper/med-data ext4$", re.M)),

    ("data-encrypted", "o backing de /data é LUKS2 ativo",
     "cryptsetup status med-data 2>&1",
     rx(r"is active[\s\S]*type:\s*LUKS2|type:\s*LUKS2[\s\S]*is active")),
]

EEG_CHECKS = [
    # Stricter than the platform's rauc-available, and it asserts something the
    # platform image cannot: on a device profile the update service is *active*,
    # which on a D-Bus activated unit means something actually spoke to it. That
    # something is MedicalUpdate, so this is evidence the framework's update
    # wrapper reached the daemon - not merely that the daemon exists.
    ("rauc-active", "MedicalUpdate ativou o serviço de atualização via D-Bus",
     "systemctl is-active rauc.service", rx(r"^active$", re.M)),

    ("config-seal", "eeg.conf casa com o sidecar sha256",
     'test "$(sha256sum /etc/medplatform/eeg.conf | cut -d" " -f1)" '
     '= "$(cat /etc/medplatform/eeg.conf.sha256)" && echo SEAL_OK || echo SEAL_BAD',
     rx(r"SEAL_OK")),

    ("config-substituted", "nenhum placeholder @MED_*@ sobrou em eeg.conf",
     "grep -c '@MED_' /etc/medplatform/eeg.conf || true", rx(r"^0$", re.M)),

    ("config-seal-owner", "o sidecar do selo pertence a root:root",
     "stat -c '%U:%G %a' /etc/medplatform/eeg.conf.sha256", rx(r"^root:root 640$", re.M)),

    ("acq-active", "o serviço de aquisição está rodando, sem reinícios",
     "systemctl show eeg-acquisition.service -p ActiveState -p NRestarts",
     rx(r"ActiveState=active[\s\S]*NRestarts=0|NRestarts=0[\s\S]*ActiveState=active")),

    ("acq-socket", "o socket de amostras foi publicado",
     "test -S /run/medplatform/eeg.sock && echo OK", rx(r"OK")),

    ("acq-realtime", "o kernel concedeu SCHED_RR prioridade 50",
     "chrt -p $(pidof eeg-acquisition-service) 2>&1",
     rx(r"SCHED_RR[\s\S]*priority:\s*50")),

    # The framework's own acceptance test. MedicalStorage resolves the device
    # behind /data and refuses to open the store unless its dm/uuid starts with
    # CRYPT-; with require_encryption=true, the service running at all is the
    # assertion. This check confirms the requirement is actually switched on -
    # otherwise the one above proves the volume exists and nothing proves the
    # application cares.
    ("storage-requires-encryption", "a aplicação exige backing criptografado",
     "grep '^storage.require_encryption' /etc/medplatform/eeg.conf",
     rx(r"=\s*true")),

    ("no-encryption-waiver", "nenhum registro de auditoria dispensando criptografia",
     "journalctl -u eeg-acquisition -b --no-pager | grep -c 'encryption requirement disabled' || true",
     rx(r"^0$", re.M)),

    ("sandbox-exposure", "o sandbox do serviço não regrediu",
     "systemd-analyze security eeg-acquisition.service --no-pager 2>&1 | tail -3",
     check_exposure),
]

PROFILES = {
    "eeg": ("make runqemu", PLATFORM_CHECKS + EEG_CHECKS),
    "tomograph": ("make runqemu-tomograph", PLATFORM_CHECKS),
}


class Console:
    def __init__(self, argv):
        self.master, slave = pty.openpty()
        env = dict(os.environ, TERM="dumb")
        self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                                     cwd=ROOT, env=env, preexec_fn=os.setsid,
                                     close_fds=True)
        os.close(slave)
        self.buf = ""
        self.log = []

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.master], [], [], 0.4)
            if not ready:
                continue
            try:
                data = os.read(self.master, 65536)
            except OSError:
                return False
            if not data:
                return False
            text = data.decode("utf-8", "replace")
            self.log.append(text)
            self.buf += text
        return True

    def expect(self, pattern, timeout, label):
        compiled = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            if compiled.search(self.buf):
                return True
            if not self.pump(1.0):
                break
        print(f"  ! timeout aguardando {label}", file=sys.stderr)
        return False

    def send(self, line):
        os.write(self.master, (line + "\n").encode())

    def run(self, cid, command):
        """Run a command between unique markers and return its output.

        The parsing has to account for the tty echoing the command line back.
        If the marker the shell *prints* is spelled the same way as the marker
        in the command that gets echoed, then a search finds the echo first: the
        captured body is the tail of the command line, and the wait for the end
        marker returns before the command has even run.

        Anchoring the marker to the start of a line does not fix it either - the
        console is 80 columns and these command lines wrap.

        So the two spellings are made different. "'@@B:id@''@'" is two adjacent
        quoted strings, which the shell concatenates into @@B:id@@ on output
        while the echoed line keeps the quotes. Searching for the concatenated
        form therefore matches the output and never the echo.
        """
        self.buf = ""
        begin, end = f"@@B:{cid}@@", f"@@E:{cid}@@"
        b_lit, e_lit = f"'@@B:{cid}@''@'", f"'@@E:{cid}@''@'"
        self.send(f"echo {b_lit}; {command} 2>&1; echo {e_lit}")
        if not self.expect(re.escape(end), CMD_TIMEOUT, f"fim de '{cid}'"):
            return None

        body = self.buf.split(begin, 1)[-1].split(end, 1)[0]
        return "\n".join(l.rstrip("\r") for l in body.splitlines()).strip()

    def close(self):
        try:
            self.proc.terminate()
        except Exception:
            pass
        # The container outlives the docker client: terminating make leaves qemu
        # running and port 2222 bound, which breaks the next run.
        subprocess.run(
            f"for c in $(docker ps -q --filter ancestor={KAS_IMAGE}); "
            "do docker stop -t 5 $c >/dev/null 2>&1; done",
            shell=True)


def main():
    profile = sys.argv[1] if len(sys.argv) > 1 else "eeg"
    if profile not in PROFILES:
        print(f"perfil desconhecido '{profile}' (use: {', '.join(PROFILES)})", file=sys.stderr)
        return 2
    target, checks = PROFILES[profile]

    argv = target.split()
    if os.environ.get("NATIVE") == "1":
        argv.append("NATIVE=1")

    print(f"== MedPlatform :: suite de aceitação em runtime :: perfil {profile} ==\n")
    print("bootando a imagem QEMU ...", flush=True)
    con = Console(argv)
    results = []
    try:
        if not con.expect(r"login:|Control-D to continue", BOOT_TIMEOUT, "prompt de login"):
            print("\nFALHA: a imagem não chegou a um prompt.", file=sys.stderr)
            return 2
        if "Control-D to continue" in con.buf:
            print("\nFALHA: o boot caiu em modo de emergência.", file=sys.stderr)
            return 2

        # Log in, nudging with a newline: the console can be busy at this point
        # and a single "root" is not guaranteed to land.
        for attempt in range(12):
            con.buf = ""
            con.send("root" if attempt == 0 else "")
            if con.expect(r"[#$] $|[#$]\s*$", 15, "shell"):
                break
        else:
            print("\nFALHA: não consegui logar.", file=sys.stderr)
            return 2

        # Silence the kernel console and pin the prompt, so parsing depends on
        # our markers only.
        con.buf = ""
        con.send(f"dmesg -n 1 2>/dev/null; PS1='{PROMPT}\\n'; export PS1")
        con.expect(re.escape(PROMPT), 30, "prompt fixado")

        print("executando verificações ...\n", flush=True)
        for cid, claim, command, predicate in checks:
            out = con.run(cid, command)
            if out is None:
                ok, detail = False, "sem resposta do guest"
            else:
                ok, detail = predicate(out)
            results.append((cid, claim, ok, detail))
            print(f"  {'PASS' if ok else 'FAIL'}  {cid:<24} {claim}")
            if not ok:
                print(f"        -> {detail}")

        con.buf = ""
        con.send("poweroff")
        con.pump(45)
    finally:
        con.close()

    failed = [r for r in results if not r[2]]
    print(f"\n{len(results) - len(failed)}/{len(results)} verificações passaram.")
    if failed:
        print("falharam: " + ", ".join(r[0] for r in failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
