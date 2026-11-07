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

Usage:  make check                            the EEG device profile (all assertions)
        python3 scripts/med-check.py tomograph  the tomograph profile (platform
                                                assertions only; no make target,
                                                it is a control case and not part
                                                of the routine loop)
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
EXPECTED_FAILED = set()
# weston.service used to be listed here, with the reason "nographic: there is no
# DRM/KMS device for a compositor to open". That reason stopped being true when
# meta-med-distro started shipping a seatd.service: weston now reaches
# ActiveState=active with Result=success and NRestarts=0 under nographic. An
# allowlist entry whose justification has expired is a muted alarm - it would
# have swallowed a real compositor failure silently - so it is gone, and
# weston starting is now something the suite asserts rather than excuses.


def check_failed_units(out):
    got = {line.split()[0] for line in out.splitlines() if line.strip()}
    unexpected = got - EXPECTED_FAILED
    if unexpected:
        return False, "unidades falhas não previstas: " + ", ".join(sorted(unexpected))
    return True, "nenhuma além das previstas" if got else "nenhuma"


def check_journal_integrity(out):
    """Every *archived* journal file must verify.

    The assertion this replaces ran `journalctl --verify | tail -1` and matched
    ^PASS. That was sound only while the journal was a single file. As soon as
    it rotates, --verify emits one line per file in an order it does not
    promise, and tail -1 asserts on whichever landed last - so the check became
    a coin flip rather than a verification. It was found by a change that made
    the system log enough to rotate, which is to say: by accident.

    The active file (system.journal) is excluded deliberately and that is the
    one judgement call here. systemd is appending to it while --verify reads it,
    so its tail is legitimately mid-write and it reports "Bad message"; the
    archived files are complete and are what an auditor would be handed. If the
    exclusion ever hides a real defect it will hide it in the newest file only,
    and the next rotation surfaces it.

    Note also what the old name claimed and this one does not. "Forward Secure
    Sealing valida" was never what ran: verifying seals needs --verify-key=,
    which this command does not pass. What is measured here is structural
    integrity of the files that carry no seal.

    Since 2026-11-02 the journal really is sealed (med-journal-seal.service),
    and a sealed file verified without the key reports "Required key not
    available" - observed on the first run after the fix, 38/39 with this
    check the only failure. That line is not corruption and is not counted as
    one here; it is not counted as a pass either. Sealed files belong to
    journal-sealed, which verifies them with the device's key and checks that
    a wrong key is refused. Any other FAIL reason on an archived file still
    fails this check.
    """
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    passed = [l for l in lines if l.startswith("PASS:")]
    failed = [l for l in lines if l.startswith("FAIL:")]
    archived_failed = [l for l in failed if not l.split()[1].endswith("/system.journal")]
    sealed = [l for l in archived_failed if l.endswith("(Required key not available)")]
    broken = [l for l in archived_failed if l not in sealed]

    if broken:
        return False, "journal arquivado não verifica: " + "; ".join(broken)
    if not passed and not sealed:
        return False, "nenhum journal verificou (saída: %s)" % (lines or "(vazia)")
    return True, "%d sem selo verificam; %d selado(s), para journal-sealed; %d ativo(s) ignorado(s)" % (
        len(passed), len(sealed), len(failed) - len(archived_failed))


# The journal's Forward Secure Sealing (med-journal-seal.service, RESULTS.md
# section 5, correction of 2026-11-02). The verification key must leave the
# device, so the suite plays the factory station: on the boot that provisions a
# machine-id it collects the key from the tmpfs hand-over file and keeps it on
# the host, under the build directory, one file per machine-id. The QEMU disk
# persists between runs, so later runs verify with the key collected earlier.
# A machine-id whose key nobody collected cannot have its seals verified by
# anyone, and the suite says so rather than passing.
SEAL_KEYS_DIR = os.path.join(os.environ.get("KAS_BUILD_DIR", os.path.join(ROOT, "build")),
                             "med-journal-keys")
SEAL = {}


def check_seal_key(out):
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    if not lines or not re.fullmatch(r"[0-9a-f]{32}", lines[0]):
        return False, "não li o machine-id (saída: %s)" % (lines or "(vazia)")
    mid = lines[0]
    path = os.path.join(SEAL_KEYS_DIR, mid + ".key")
    handed = lines[1] if len(lines) > 1 and lines[1] != "NOKEY" else None
    if handed:
        if not re.fullmatch(r"[0-9a-f-]+/[0-9a-f]+-[0-9a-f]+", handed):
            return False, "a chave entregue não tem a forma de uma chave de verificação"
        os.makedirs(SEAL_KEYS_DIR, mode=0o700, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(handed + "\n")
    if not os.path.exists(path):
        return False, ("a chave de verificação de %s nunca foi recolhida: os selos desta "
                       "identidade não podem ser verificados" % mid)
    with open(path) as f:
        SEAL["key"] = f.read().strip()
    SEAL["mid"] = mid
    return True, ("recolhida agora" if handed else "recolhida num boot anterior") + \
        " (%s)" % path


def wrong_key(key):
    # Same form, one hex digit of the seed changed: a key journalctl accepts as
    # a key and that is not this device's. A malformed one would be refused for
    # its form, and "refused" would then prove nothing about the seal.
    i = 0
    return ("0" if key[i] != "0" else "1") + key[i + 1:]


def seal_command():
    key = SEAL.get("key")
    if not key:
        return "echo 'sem chave recolhida'"
    return (
        "M=$(cat /etc/machine-id); journalctl --rotate >/dev/null 2>&1; sleep 2; "
        "n=0; ok=0; ref=0; "
        "for f in /var/log/journal/$M/system@*.journal; do "
        "journalctl --header --file=\"$f\" 2>/dev/null | grep -q '^Compatible flags:.*SEALED' || continue; "
        "n=$((n+1)); "
        "journalctl --verify --verify-key=%s --file=\"$f\" >/dev/null 2>&1 && ok=$((ok+1)); "
        "journalctl --verify --verify-key=%s --file=\"$f\" >/dev/null 2>&1 || ref=$((ref+1)); "
        "done; echo \"sealed=$n ok=$ok wrongkey_refused=$ref\"" % (key, wrong_key(key)))


def check_sealed(out):
    m = re.search(r"sealed=(\d+) ok=(\d+) wrongkey_refused=(\d+)", out)
    if not m:
        return False, out.strip().splitlines()[-1] if out.strip() else "(vazio)"
    n, ok, ref = map(int, m.groups())
    if n == 0:
        return False, "nenhum journal arquivado selado"
    if ok != n:
        return False, "%d de %d selados não verificam com a chave do dispositivo" % (n - ok, n)
    if ref != n:
        return False, "%d de %d selados verificam com uma chave errada" % (n - ref, n)
    return True, "%d selado(s): todos verificam com a chave, nenhum com uma errada" % n


def check_audit_chain(out):
    """The trail as written, and a copy with one field edited.

    The first half reads the device's own audit records through the verifier
    the framework ships. The second edits the first MESSAGE of a copy and runs
    the same verifier over it: an assertion about tamper evidence that has
    never seen tampering is a claim (implementation_plan_conformity.md, 2.1).
    """
    m = re.search(r"entries=(\d+) components=(\d+) breaks=(\d+)", out)
    rc = re.search(r"rc=(\d+)", out)
    edited = re.search(r"edited_rc=(\d+)", out)
    if not m or not rc or not edited:
        return False, out.strip().splitlines()[-1] if out.strip() else "(vazio)"
    entries, components, breaks = map(int, m.groups())
    verified = sum(int(v) for v in re.findall(r"verified=(\d+)", out))
    legacy = sum(int(v) for v in re.findall(r"legacy=(\d+)", out))
    if breaks or rc.group(1) != "0":
        first = re.search(r"^break .*$", out, re.M)
        return False, "%d quebra(s): %s" % (breaks, first.group(0) if first else "?")
    if verified == 0:
        return False, "nenhum registro no formato 2 foi verificado (%d entradas)" % entries
    if edited.group(1) != "1":
        return False, "a cópia com um campo alterado passou no verificador (rc=%s)" % edited.group(1)
    return True, ("%d registro(s) em %d cadeia(s), %d verificados, %d legado; "
                  "a cópia alterada foi recusada" % (entries, components, verified, legacy))


# Listeners an image may have outside the loopback, each with its reason - the
# same rule as EXPECTED_FAILED: an allowance nobody justifies is a muted alarm.
# Every image this suite runs is a development image; a product image must
# leave this list empty of 22.
ALLOWED_LISTENERS = {
    "tcp:22": "sshd.socket, perfil de desenvolvimento (med-image-dev.inc); some no produto",
    "udp:68": "cliente DHCP do systemd-networkd",
}


def check_attack_surface(out):
    """IEC 81001-5-1 5.7.3 b): the ports the image exposes, and its root services.

    Every listener off the loopback must be declared in ALLOWED_LISTENERS; the
    loopback ones (the local resolver) are reported. Services running as root
    are counted and reported - that is ACE-02's measurement, not a pass/fail here.
    """
    exposed, local = [], []
    for line in out.splitlines():
        cols = line.split()
        if len(cols) < 4 or cols[0] not in ("tcp", "tcp6", "udp", "udp6"):
            continue
        address, _, port = cols[3].rpartition(":")
        key = "%s:%s" % (cols[0].rstrip("6"), port)
        if address.startswith("127.") or address in ("::1", "[::1]"):
            local.append(key)
        else:
            exposed.append(key)
    if not exposed and not local:
        return False, "não li nenhum socket (netstat falhou?)"
    surprise = sorted(set(exposed) - set(ALLOWED_LISTENERS))
    roots = re.findall(r"^(\S+\.service)=(\S*)$", out, re.M)
    as_root = [u for u, user in roots if user in ("", "0", "root")]
    detail = "expostas %s; loopback %s; %d de %d serviços como root" % (
        ",".join(sorted(set(exposed))), ",".join(sorted(set(local))), len(as_root), len(roots))
    if surprise:
        return False, "porta exposta não declarada: %s (%s)" % (",".join(surprise), detail)
    return True, detail


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

    ("attack-surface", "nenhuma porta exposta além das declaradas (81001-5-1 §5.7.3 b)",
     "netstat -ltun; for u in $(systemctl list-units --type=service --state=running --no-legend "
     "--plain | awk '{print $1}'); do echo $u=$(systemctl show -p User --value $u); done",
     check_attack_surface),

    ("failed-units", "nenhuma unidade falha além das previstas",
     "systemctl list-units --state=failed --no-legend --plain | awk '{print $1}'",
     check_failed_units),

    # Persistent is not enough: a trail on the rootfs is inside an A/B slot, and
    # the next update rewrites it (AUD-06). Both must be bind mounts of the
    # /data volume, which no update writes (implementation_plan_conformity.md
    # 5.1). The VOLATILE_LOG_DIR regression this assertion was first written for
    # (/var/log a symlink to a tmpfs) is still checked by the NOLINK half.
    ("journal-persistent", "/var/log/journal está no volume /data, fora dos slots A/B",
     # mountinfo, not findmnt: the image carries only parts of util-linux, and a
     # missing command prints nothing - which this assertion first read as "not
     # mounted" on an image where the mount was there.
     "awk '$5==\"/var/log/journal\" {print \"src=\" $(NF-1) \" root=\" $4}' /proc/self/mountinfo; "
     "test ! -L /var/log && echo NOLINK",
     rx(r"src=/dev/mapper/med-data root=/state/journal[\s\S]*NOLINK")),

    ("state-persistent", "/var/lib/medplatform está no volume /data, fora dos slots A/B",
     "awk '$5==\"/var/lib/medplatform\" {print \"src=\" $(NF-1) \" root=\" $4}' /proc/self/mountinfo; "
     "echo end",
     rx(r"src=/dev/mapper/med-data root=/state/medplatform")),

    # O --rotate é o que torna esta asserção determinística. Sem ele o resultado
    # depende de o journal ter rotacionado sozinho durante o boot: numas rodadas
    # havia arquivados para verificar, noutras só o arquivo ativo - cuja
    # verificação falha ou passa conforme o systemd esteja escrevendo nele
    # naquele instante. Rotacionar fecha o arquivo corrente e garante que existe
    # pelo menos um journal completo para verificar de verdade.
    ("journal-integro", "todo journal arquivado verifica íntegro",
     "journalctl --rotate >/dev/null 2>&1; sleep 2; "
     "journalctl --verify 2>&1 | grep -E '^(PASS|FAIL):' || true",
     check_journal_integrity),

    # Forward Secure Sealing, which journal-integro does not see: without the
    # key, --verify checks structure only. The first collects the verification
    # key (and fails if this identity's was never collected); the second needs
    # it, so it is built when it runs.
    ("journal-seal-key", "a chave de verificação do selo saiu do dispositivo",
     "cat /etc/machine-id; cat /run/med-journal-seal/verify-key 2>/dev/null || echo NOKEY",
     check_seal_key),

    ("journal-sealed", "o journal é selado e o selo só valida com a chave do dispositivo",
     seal_command, check_sealed),

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

    # The display half of the device, asserted the same way the acquisition half
    # is and for the same reason: this unit has Restart=on-failure, so
    # ActiveState alone reports a crash loop as healthy. It was in one -
    # XDG_RUNTIME_DIR named /run/user/0 while weston publishes /run/wayland-0 -
    # for as long as this suite has existed, and the suite had no assertion that
    # could see it. An EEG monitor whose display never starts is not a partial
    # device; it is a different device.
    # A espera é limitada e não é complacência: o Qt leva ~6 s para inicializar
    # (carregar o plugin wayland, varrer fontes), e medir logo após o boot pega
    # ActiveState=activating de um serviço perfeitamente saudável. O que a
    # espera *não* mascara é o caso que importa: um crash loop continua tendo
    # NRestarts > 0 depois dela, e 30 s é mais que os ~7 s de um ciclo de
    # reinício desta unidade.
    ("hmi-active", "a HMI de operador está rodando, sem reinícios",
     "for i in $(seq 1 30); do "
     "  systemctl is-active -q eeg-hmi.service && break; sleep 1; done; "
     "systemctl show eeg-hmi.service -p ActiveState -p NRestarts",
     rx(r"ActiveState=active[\s\S]*NRestarts=0|NRestarts=0[\s\S]*ActiveState=active")),

    # O revisor de registros: de pé, sem reinícios, e como o SEU usuário. A
    # identidade é metade da asserção de propósito - a unidade tem
    # Restart=always, então "ativo" sozinho passaria um laço de quedas (por
    # isso NRestarts), e um revisor ativo como root seria uma segunda porta
    # para o registro bruto com a aparência exata de um revisor que funciona.
    #
    # A identidade é lida até coincidir, por no máximo 10 s, e não uma vez só.
    # "Ativo" numa unidade Type=simple começa no fork, e o filho ainda é root
    # até trocar de usuário e fazer o exec: lido nesse instante, um revisor
    # correto aparece com uid 0. Foi assim que esta asserção falhou num build
    # que estava certo (2026-10-11, uid=0 esperado=1201), depois de ter passado
    # várias vezes por sorte. Um revisor que roda mesmo como root continua
    # reprovando: ele nunca chega a coincidir.
    #
    # O revisor não sobe no boot: sobe quando a HMI pede, deixando um arquivo
    # num diretório de pedidos que uma unidade .path observa. A asserção
    # faz o pedido pelo mesmo caminho - então ela mede também que o pedido
    # chega, e que quem atende é o systemd e não quem pediu.
    ("review-active", "o revisor sobe a pedido da HMI, sem reinícios, como med-review",
     "echo pedido > /run/med-review-request/request; "
     "for i in $(seq 1 30); do "
     "  systemctl is-active -q med-review.service && break; sleep 1; done; "
     "W=$(id -u med-review); "
     "for i in $(seq 1 20); do "
     "  P=$(systemctl show med-review.service -p MainPID --value); "
     "  U=$(sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p' /proc/$P/status 2>/dev/null); "
     "  test \"$U\" = \"$W\" && break; sleep 0.5; done; "
     "systemctl show med-review.service -p ActiveState -p NRestarts; "
     "echo \"uid=$U esperado=$W\"; test -n \"$U\" && test \"$U\" = \"$W\" && echo REVISOR_OK",
     rx(r"(ActiveState=active[\s\S]*NRestarts=0|NRestarts=0[\s\S]*ActiveState=active)"
        r"[\s\S]*REVISOR_OK")),

    ("acq-socket", "o socket de amostras foi publicado",
     "test -S /run/medplatform/eeg.sock && echo OK", rx(r"OK")),

    # A taxa ENTREGUE contra a CONFIGURADA, e não apenas "o serviço está vivo".
    #
    # Esta asserção existe porque a suíte deu 22/22 numa imagem que entregava 94
    # das 250 amostras/canal/s configuradas: a publicação para a HMI era
    # bloqueante, o laço de aquisição parava dentro do write() e o registro saía
    # com menos amostras do que o próprio metadata.json declarava. Nenhuma das
    # outras 22 podia ver isso - todas olham estado (ativo, sem reinícios,
    # socket publicado), e o serviço estava perfeitamente ativo enquanto perdia
    # 62% das amostras. Medir estado não é medir função.
    #
    # O método é o mesmo da RESULTS.md §3: delta entre dois stat do mesmo
    # registro, e a geometria do quadro lida do eeg.conf em vez de embutida
    # aqui - senão a asserção passa a mentir no dia em que a geometria mudar,
    # que é exatamente o dia em que ela precisa funcionar.
    #
    # Tolerância de 10% e janela de 20 s: sob QEMU sem tempo real, a taxa
    # instantânea oscila; o que esta asserção procura é a ordem de grandeza
    # errada (62% de perda), não jitter.
    #
    # O quadro é cabeçalho (40), amostras e, desde a v2 do formato, o bloco de
    # 8 bytes de contato de eletrodo. Sem os 8 a asserção superestimava a taxa
    # em ~1,6% - a favor de passar, que é o lado errado para errar.
    # Nada é gravado até alguém pedir. Uma sessão já foi a vida do processo:
    # cada boot e cada reinício do serviço deixavam um registro que ninguém
    # tinha pedido - nove num dia de bancada, até 82 MiB cada. As duas
    # asserções abaixo são as duas metades da correção: ocioso depois do boot
    # (sem marcador de sessão viva, e o serviço diz "idle"), e gravando depois
    # do pedido. A segunda vem antes de tudo o que precisa de uma sessão.
    ("session-idle-at-boot", "depois do boot o serviço adquire e não grava",
     'L=$(ls /run/medplatform/session 2>/dev/null); T=$(medctl session.status); '
     'A=$(du -s /data/eeg 2>/dev/null | cut -f1); sleep 5; '
     'B=$(du -s /data/eeg 2>/dev/null | cut -f1); '
     'echo "marcador=[$L] estado=$T dados=$A->$B"; '
     'test -z "$L" && test "$T" = idle && test "$A" = "$B" && echo IDLE_OK',
     rx(r"IDLE_OK")),

    ("session-start", "uma sessão começa quando é pedida, pela linha de comando",
     'R=$(medctl session.start operator=suite exam=med-check); E=$?; '
     'T=$(medctl session.status); echo "resposta=$R saida=$E estado=$T"; '
     'test $E = 0 && echo "$T" | grep -q "^recording " && echo START_OK',
     rx(r"START_OK")),

    ("acq-sample-rate", "a taxa entregue é a configurada, com a HMI conectada",
     "C=/etc/medplatform/eeg.conf; "
     "g() { grep \"^$1\" $C | cut -d= -f2 | tr -d ' '; }; "
     "N=$(g acquisition.samples_per_frame); CH=$(g acquisition.channels); "
     "R=$(g acquisition.sample_rate_hz); FB=$((40 + CH * N * 4 + 8)); "
     "S=$(ls -d /data/eeg/session-* | tail -n 1); "
     "A=$(stat -c %s $S/raw.bin); sleep 20; B=$(stat -c %s $S/raw.bin); "
     "ACH=$(( (B - A) / FB * N / 20 )); "
     "echo \"entregue=$ACH configurado=$R quadro=${FB}B\"; "
     "test $ACH -ge $(( R * 9 / 10 )) && echo RATE_OK || echo RATE_LOW",
     rx(r"RATE_OK")),

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

    # The application's half of tamper evidence (AUD-03): the hash chain
    # MedicalLogger writes, replayed from the journal by the framework's
    # verifier, and the same verifier refusing a copy with one field edited.
    ("audit-chain", "a cadeia de auditoria verifica, e uma alteração é detectada",
     "journalctl -o export MED_AUDIT=1 > /run/med-audit.export; "
     "med-audit-verify /run/med-audit.export; echo rc=$?; "
     "awk '!d && /^MESSAGE=/{sub(/^MESSAGE=/,\"MESSAGE=edited \");d=1}1' "
     "/run/med-audit.export | med-audit-verify >/dev/null; echo edited_rc=$?; "
     "rm -f /run/med-audit.export",
     check_audit_chain),

    # O que o registro clínico precisa saber de uma sessão
    # (implementation_plan_bdf.md, Fase 1). O conversor que deriva o BDF não
    # pode tocar numa sessão viva e tem de distinguir uma sessão encerrada de
    # uma interrompida; as duas asserções abaixo verificam os dois marcadores
    # de que ele depende, no alvo, e não a intenção do código.
    ("session-live-marker", "a sessão sendo gravada está marcada como viva em /run",
     'L=$(cat /run/medplatform/session); S=$(ls -d /data/eeg/session-* | tail -n 1); '
     'echo "viva=$L recente=${S##*/session-}"; '
     'test -n "$L" && test "$L" = "${S##*/session-}" && echo LIVE_OK',
     rx(r"LIVE_OK")),

    # Perto do fim, de propósito: é a que encerra a sessão que as anteriores
    # usam - por pedido, com o serviço de pé, que é como uma sessão passou a
    # terminar. Depois dela vêm só as que usam a sessão fechada. O fechamento é comparado com o próprio
    # registro de auditoria da sessão (MED_FRAMES do AcquisitionStopped), não
    # com um número esperado: o marcador tem de dizer o mesmo que a trilha, e
    # é a concordância que se verifica.
    ("session-closed", "uma sessão encerrada tem closed.json que bate com a auditoria",
     'S=$(ls -d /data/eeg/session-* | tail -n 1); '
     'medctl session.stop >/dev/null; '
     'F=$(sed -n "s/.*\\"frames\\": \\([0-9]*\\).*/\\1/p" $S/closed.json); '
     'J=$(journalctl -b -o export MED_AUDIT_EVENT=ACQUISITION_STOPPED '
     'MED_SESSION=${S##*/session-} | sed -n "s/^MED_FRAMES=//p"); '
     'M=$(grep -c "\\"full_scale_uv\\": [0-9]" $S/metadata.json); '
     # N começa vazio de propósito. As asserções rodam todas no mesmo shell,
     # e a acq-sample-rate deixa N=14 (amostras por quadro): sem isto, um
     # marcador ausente - que é o resultado certo - era lido como "viva=[14]",
     # e a asserção reprovava um serviço que tinha feito tudo certo.
     'N=; test -e /run/medplatform/session && N=$(cat /run/medplatform/session); '
     'T=$(medctl session.status); '
     'echo "closed=$F auditoria=$J escala=$M viva=[$N] estado=$T"; '
     'test -n "$F" && test "$F" = "$J" && test "$M" = 1 && '
     'test -z "$N" && test "$T" = idle && echo CLOSED_OK',
     rx(r"CLOSED_OK")),

    # Um pedido que não pode ser atendido é recusado, e a recusa tem de ser
    # visível nos três lugares em que alguém a procuraria: no código de saída
    # de quem pediu, no texto que o serviço devolveu e na trilha de auditoria.
    # Encerrar uma sessão que não existe é o caso mais simples - e o serviço
    # tem de continuar de pé depois dele, sem reinício: uma recusa derrota o
    # pedido, não o serviço.
    ("control-refusal", "um pedido impossível é recusado, auditado, e o serviço segue",
     'R=$(medctl session.stop 2>&1); E=$?; '
     'U=$(medctl session.frobnicate 2>&1); F=$?; '
     'J=$(journalctl -b -o cat MED_AUDIT_EVENT=SECURITY_EVENT | grep -c "control request refused"); '
     'N=$(systemctl show eeg-acquisition.service -p NRestarts --value); '
     'echo "stop=[$R] saida=$E verbo=[$U] saida=$F auditadas=$J reinicios=$N"; '
     'test $E = 1 && test $F = 1 && test "$J" -ge 2 && test "$N" = 0 && echo REFUSAL_OK',
     rx(r"REFUSAL_OK")),

    # O arquivo clínico da sessão que a session-closed encerrou
    # (implementation_plan_bdf.md, Fase 3): fechar uma sessão dispara
    # o conversor, que não bloqueia a parada - daí a espera limitada pelo
    # export.json. O número de registros vem do próprio cabeçalho BDF+ (offset
    # 236, 8 octetos) e é comparado com os quadros do closed.json, que a
    # session-closed já comparou com a auditoria: três fontes, um número. O
    # selo é conferido por quem o lê, não por quem o escreveu.
    ("record-bdf", "a sessão encerrada ganhou o BDF+, selado, um registro por quadro",
     'S=$(ls -d /data/eeg/session-* | tail -n 1); '
     'for i in $(seq 1 60); do test -e $S/export.json && break; sleep 1; done; '
     'F=$(sed -n "s/.*\\"frames\\": \\([0-9]*\\).*/\\1/p" $S/closed.json); '
     'R=$(dd if=$S/record.bdf bs=1 skip=236 count=8 2>/dev/null | tr -d " "); '
     'C=$(cd $S && sha256sum -c record.bdf.sha256 2>&1); '
     'X=$(systemctl show eeg-record-export.service -p Result --value); '
     'echo "quadros=$F registros=$R selo=$C unidade=$X"; '
     'grep -q "\\"status\\": \\"converted\\"" $S/export.json && test -n "$F" && '
     'test "$F" = "$R" && test "$C" = "record.bdf: OK" && test "$X" = success && '
     'echo BDF_OK',
     rx(r"BDF_OK")),

    # O que o revisor alcança, medido tentando - como med-review, e não lendo
    # permissões (implementation_plan_hmi_desktop.md, Fase A3). O arquivo
    # clínico da sessão que a record-bdf acabou de conferir tem de abrir pelo
    # nome publicado; o registro bruto, o metadado e a própria árvore de
    # sessões têm de ser recusados. As quatro pernas são a asserção: um
    # revisor que não lê nada passa em três delas, e um que lê tudo passa na
    # outra. Depois da record-bdf, que é quem garante que o BDF+ existe.
    ("review-reach", "o revisor lê o BDF+ publicado e nada mais da sessão",
     'S=$(ls -d /data/eeg/session-* | tail -n 1); I=${S##*/session-}; '
     'P=/data/review/eeg/$I.bdf; '
     'for i in $(seq 1 30); do test -e "$P" && break; sleep 1; done; '
     'A=$(su -s /bin/sh med-review -c "dd if=\'$P\' bs=1 count=8 2>/dev/null | wc -c"); '
     'B=$(su -s /bin/sh med-review -c "cat \'$S/raw.bin\' >/dev/null 2>&1 && echo LEU || echo NEGADO"); '
     'C=$(su -s /bin/sh med-review -c "cat \'$S/metadata.json\' >/dev/null 2>&1 && echo LEU || echo NEGADO"); '
     'D=$(su -s /bin/sh med-review -c "ls /data/eeg >/dev/null 2>&1 && echo LEU || echo NEGADO"); '
     'L=$(stat -c %h "$S/record.bdf"); '
     'echo "bdf=$A bruto=$B metadado=$C sessoes=$D nomes=$L"; '
     'test "$A" = 8 && test "$B" = NEGADO && test "$C" = NEGADO && test "$D" = NEGADO && '
     'test "$L" = 2 && echo REVISAO_OK',
     rx(r"REVISAO_OK")),

    # O usuário do ambiente de trabalho, que numa estação é o revisor
    # (implementation_plan_desktop.md, Fase D1): o que o gerenciador de
    # arquivos da barra pode abrir é o que este usuário pode. Os grupos são
    # lidos da unidade e não repetidos aqui, para que a asserção siga a
    # unidade. Três pernas: lê o BDF+ publicado, não lê o registro bruto, e
    # não escreve onde lê. Numa imagem sem o ambiente (a de tela única) não
    # há o que medir, e a saída diz isso em vez de dizer OK calado.
    ("desktop-reach", "o usuário do ambiente lê o BDF+ publicado, não o bruto, e não escreve",
     'systemctl cat med-desktop >/dev/null 2>&1 || { echo "sem ambiente: AMBIENTE_OK"; exit 0; }; '
     'S=$(ls -d /data/eeg/session-* | tail -n 1); P=/data/review/eeg/${S##*/session-}.bdf; '
     'G=$(systemctl show -p SupplementaryGroups --value med-desktop); '
     'R="systemd-run -q --wait --pipe -p User=med-desktop -p ProtectSystem=strict"; '
     'A=$($R -p "SupplementaryGroups=$G" /bin/sh -c "dd if=\'$P\' bs=1 count=8 2>/dev/null | wc -c"); '
     'B=$($R -p "SupplementaryGroups=$G" /bin/sh -c "cat \'$S/raw.bin\' >/dev/null 2>&1 && echo LEU || echo NEGADO"); '
     'C=$($R -p "SupplementaryGroups=$G" /bin/sh -c "touch /data/review/eeg/x 2>/dev/null && echo ESCREVEU || echo NEGADO"); '
     'echo "grupos=[$G] bdf=$A bruto=$B escrita=$C"; '
     'test "$A" = 8 && test "$B" = NEGADO && test "$C" = NEGADO && echo AMBIENTE_OK',
     rx(r"AMBIENTE_OK")),

    # Uma configuração do front-end escolhida em runtime
    # (implementation_plan_hmi_desktop.md, Fase C). A última, porque abre e
    # fecha uma sessão própria. Sete pernas, e cada uma é um jeito de isto
    # estar errado: a escolha é aceita; um valor fora da tabela selada é
    # recusado; uma troca no meio de uma sessão é recusada; a sessão gravada
    # sob a escolha DECLARA a escolha (fundo de escala e prescrição) e diz
    # que não é a de fábrica; a troca foi auditada; o serviço não reiniciou;
    # e a configuração de fábrica volta ao ser pedida.
    #
    # No link simulado o fundo de escala é a referência sobre o ganho,
    # 4500000 / 12 = 375000 uV - um número que só aparece se o dispositivo
    # foi de fato reaberto com as opções da linha escolhida.
    ("configure-choice", "uma configuração do front-end é escolhida, ou recusada com motivo",
     'A=$(medctl session.configure afe.gain=12); EA=$?; '
     'B=$(medctl session.configure afe.gain=3 2>&1); EB=$?; '
     'medctl session.start exam=configure >/dev/null; sleep 3; '
     'C=$(medctl session.configure afe.gain=24 2>&1); EC=$?; '
     'medctl session.stop >/dev/null; Q=$(medctl device.prescription); '
     'echo "aceita=[$A]/$EA fora=[$B]/$EB aberta=[$C]/$EC agora=[$Q]"; '
     'test $EA = 0 && test $EB = 1 && test $EC = 1 && test "$Q" = "$A" && echo CONFIGURE_OK',
     rx(r"CONFIGURE_OK")),

    # A segunda metade, sobre a sessão que a de cima gravou: o registro
    # declara a escolha, a troca foi auditada, a de fábrica volta, e o
    # serviço não reiniciou em nenhum momento. Separada da primeira por uma
    # razão que não é de projeto: as duas juntas passavam de 1024 caracteres,
    # que é quanto o shell do alvo aceita numa linha - a linha chegava cortada,
    # o shell ficava esperando o resto, e a asserção reprovava por "sem
    # resposta do guest" um serviço que tinha feito tudo certo.
    ("configure-record", "a sessão gravada sob uma escolha a declara, e a de fábrica volta",
     'S=$(ls -d /data/eeg/session-* | tail -n 1); '
     'F=$(sed -n "s/.*\\"full_scale_uv\\": \\([0-9.]*\\).*/\\1/p" $S/metadata.json); '
     'G=$(grep -c "\\"afe.gain\\": 12" $S/metadata.json); '
     'P=$(grep -c "operator choice" $S/metadata.json); '
     'J=$(journalctl -b -o cat MED_AUDIT_EVENT=CONFIGURATION_CHANGED | grep -c "prescription changed"); '
     'D=$(medctl session.configure afe.gain=24); ED=$?; Q=$(medctl device.prescription); '
     'N=$(systemctl show eeg-acquisition.service -p NRestarts --value); '
     'F0=$(grep "^choices.factory" /etc/medplatform/eeg-choices.conf | tr -dc 0-9); '
     'echo "escala=$F ganho=$G fonte=$P auditadas=$J volta=[$D]/$ED agora=[$Q] fabrica=$F0 reinicios=$N"; '
     'test "$F" = 375000 && test "$G" = 1 && test "$P" = 1 && test "$J" -ge 1 && '
     'test $ED = 0 && test "$N" = 0 && test "$Q" = "choice $F0" && echo RECORD_OK',
     rx(r"RECORD_OK")),

    # O canal que não é eletrodo (implementation_plan_hmi_desktop.md, Fase D).
    # Uma sessão com o sensor de temperatura no último canal: o registro
    # declara o canal, o arquivo clínico sai com sete sinais EEG e o oitavo
    # rotulado "Temp", em degC. Depois o sensor é desligado. No link simulado
    # o nível é o que a tabela diz que o sensor lê a 27 graus; isto mede a
    # declaração e a conversão, não um sensor.
    ("configure-temperature", "com o sensor no canal 8 a sessão segue EEG nos outros sete",
     'medctl session.configure afe.temperature=true >/dev/null; EA=$?; '
     'medctl session.start >/dev/null; sleep 3; medctl session.stop >/dev/null; '
     'S=$(ls -d /data/eeg/session-* | tail -n 1); '
     'for i in $(seq 1 30); do test -e $S/record.bdf.sha256 && break; sleep 1; done; '
     'C=$(grep -c "\\"auxiliary_channel\\": 8" $S/metadata.json); '
     'L1=$(dd if=$S/record.bdf bs=1 skip=256 count=5 2>/dev/null); '
     'L8=$(dd if=$S/record.bdf bs=1 skip=368 count=6 2>/dev/null); '
     'D1=$(dd if=$S/record.bdf bs=1 skip=1120 count=2 2>/dev/null); '
     'D8=$(dd if=$S/record.bdf bs=1 skip=1176 count=4 2>/dev/null); '
     'medctl session.configure afe.temperature=false >/dev/null; EB=$?; '
     'echo "ped=$EA canal=$C rot=[$L1][$L8] dim=[$D1][$D8] volta=$EB"; '
     'test $EA = 0 && test "$C" = 1 && test "$L1" = "EEG 1" && test "$L8" = "Temp 8" && '
     'test "$D1" = uV && test "$D8" = degC && test $EB = 0 && echo TEMPERATURA_OK',
     rx(r"TEMPERATURA_OK")),

    # A taxa de amostragem escolhida (implementation_plan_hmi_desktop.md,
    # §12.10). Quatro pernas: uma taxa que o conversor não tem é recusada; uma
    # que a tabela deste link lista é aceita; a sessão gravada sob ela a
    # DECLARA e a CONTÉM - contada no registro, como na acq-sample-rate, e não
    # lida do cabeçalho, porque um registro que declara 1000 e contém 250 é o
    # defeito que esta asserção existe para ver; e a de fábrica volta. Os
    # limites são dos dois lados: abaixo de 90% é perda, acima de 110% é uma
    # taxa que não é a pedida. Só tem o que medir num link cuja tabela lista
    # mais de uma taxa. Reprovou no QEMU em 2026-10-13 com entregue=858:
    # cada quadro era sincronizado no volume de registros antes de o seguinte
    # ser lido, ~61 quadros por segundo. É o que o RecordStream do framework
    # resolve, e esta asserção é a que diz se resolveu.
    ("configure-rate", "a taxa escolhida é a que o registro declara e a que ele contém",
     'K=acquisition.sample_rate_hz; R0=$(grep "^$K" /etc/medplatform/eeg.conf | tr -dc 0-9); '
     'medctl session.configure $K=300 >/dev/null 2>&1; EX=$?; '
     'medctl session.configure $K=1000 >/dev/null; EA=$?; '
     'medctl session.start exam=rate >/dev/null; sleep 2; '
     'S=$(ls -d /data/eeg/session-* | tail -n 1); '
     'A=$(stat -c %s $S/raw.bin); sleep 10; B=$(stat -c %s $S/raw.bin); '
     'medctl session.stop >/dev/null; '
     'D=$(grep -c "\\"sample_rate_hz\\": 1000\\." $S/metadata.json); '
     'ACH=$(( (B - A) / 496 * 14 / 10 )); '
     'medctl session.configure $K=$R0 >/dev/null; EB=$?; '
     'echo "fora=$EX ped=$EA declarada=$D entregue=$ACH volta=$EB para=$R0"; '
     'test $EX = 1 && test $EA = 0 && test "$D" = 1 && test $ACH -ge 900 && '
     'test $ACH -le 1100 && test $EB = 0 && echo TAXA_OK',
     rx(r"TAXA_OK")),

    # A reserva do volume de registros, com o volume quase cheio de verdade:
    # um arquivo de enchimento que deixa 16 MiB livres, metade da reserva. Não
    # até o último octeto - a reserva existe para que o serviço feche a sessão
    # ENQUANTO ainda há onde escrever o marcador de fechamento, e um disco
    # enchido de fora num segundo não é o caso que ela cobre. Três pernas - uma sessão em
    # curso é FECHADA pelo serviço, como registro completo e com o motivo no
    # marcador; uma sessão nova é recusada; e, liberado o espaço, o serviço
    # volta a aceitar sem ter reiniciado. Existe porque isto aconteceu na
    # placa em 2026-10-11 sem reserva nenhuma: quadros perdidos, um arquivo
    # clínico pela metade e uma atualização que não instalava, porque o
    # atualizador guarda o estado dele no mesmo volume.
    ("storage-reserve", "com o volume cheio a sessão fecha inteira e outra é recusada",
     'medctl session.start exam=reserve >/dev/null; sleep 2; '
     'S=$(ls -d /data/eeg/session-* | tail -n 1); '
     'A=$(df -m /data | tail -n 1 | tr -s " " | cut -d" " -f4); '
     'dd if=/dev/zero of=/data/enchimento bs=1M count=$((A - 16)) 2>/dev/null; sync; '
     'for i in $(seq 1 20); do test -e $S/closed.json && break; sleep 1; done; '
     'C=$(grep -c "record volume full" $S/closed.json 2>/dev/null); '
     'R=$(medctl session.start exam=cheio 2>&1); E=$?; '
     'rm -f /data/enchimento; sync; '
     'G=$(medctl session.start exam=livre); EG=$?; medctl session.stop >/dev/null; '
     'N=$(systemctl show eeg-acquisition.service -p NRestarts --value); '
     'echo "fechada=$C recusa=[$R]/$E depois=[$G]/$EG reinicios=$N"; '
     'test "$C" = 1 && test $E = 1 && test $EG = 0 && test "$N" = 0 && echo RESERVE_OK',
     rx(r"RESERVE_OK")),
]

# ------------------------------------------------- infusion pump (phase 7)
#
# Each scenario ends with the controller idle and no alarm, so the next one
# starts from the state the previous one found. Every number a predicate checks
# is the model's (docs/REQUISITOS_INFUSAO.md section 2), never a real pump's.

def _kv(out):
    """Every key=value token of the output; the last one of a key wins."""
    values = {}
    for token in re.split(r"\s+", out or ""):
        if "=" in token:
            key, _, value = token.partition("=")
            values[key] = value
    return values


def _after(out, marker):
    return out.split(marker, 1)[1] if marker in (out or "") else ""


def _elapsed(v):
    try:
        return float(v["t1"]) - float(v["t0"])
    except (KeyError, ValueError):
        return None


def check_ders_hard(out):
    v = _kv(out)
    if v.get("rc") != "1" or "hard limit" not in out:
        return False, "a taxa acima do limite rígido não foi recusada (rc=%s)" % v.get("rc")
    if v.get("audit") != "1":
        return False, "a recusa não deixou um SAFETY_LIMIT_VIOLATION (%s)" % v.get("audit")
    if v.get("state") != "idle":
        return False, "o controlador saiu de idle: %s" % v.get("state")
    return True, "recusada, auditada, nada programado"


def check_ders_soft(out):
    v = _kv(out)
    if v.get("rc1") != "1":
        return False, "acima do limite flexível foi aceito sem confirmação"
    if v.get("rc2") != "0" or v.get("state") != "programmed":
        return False, "a confirmação de um operador não foi aceita (rc2=%s)" % v.get("rc2")
    if v.get("audit") != "1":
        return False, "a confirmação não deixou um SAFETY_LIMIT_OVERRIDDEN com o operador"
    return True, "recusado sem confirmação; aceito com ela, e o operador está na trilha"


def _iso(text):
    import datetime
    return datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%fZ")


def check_delivery_volume(out):
    v = _kv(out)
    try:
        elapsed = (_iso(v["THERAPY_STOPPED"]) - _iso(v["THERAPY_STARTED"])).total_seconds()
    except (KeyError, ValueError):
        elapsed = None
    try:
        delivered = float(v["delivered_ml"])
    except (KeyError, ValueError):
        return False, "sem delivered_ml"
    if elapsed is None or elapsed <= 0:
        return False, "sem os registros de início e pausa da terapia"
    expected = 360.0 / 3600.0 * elapsed
    error = abs(delivered - expected) / expected
    detail = "%.4f mL em %.2f s, esperado %.4f (%.2f%%)" % (delivered, elapsed, expected, 100 * error)
    return error <= 0.03, detail


def check_vtbi(out):
    first, last = out.split("@@end", 1) if "@@end" in (out or "") else (out, "")
    v, w = _kv(first), _kv(last)
    try:
        delivered = float(v["delivered_ml"])
    except (KeyError, ValueError):
        return False, "sem delivered_ml"
    if v.get("state") != "completed" or "vtbi_complete:medium" not in v.get("alarms", ""):
        return False, "não concluiu: state=%s alarms=%s" % (v.get("state"), v.get("alarms"))
    if v.get("motor") != "0":
        return False, "concluída com o motor acionado"
    bound = 0.5 + 400.0 / 3600.0 * 0.1
    if not 0.5 <= delivered <= bound + 1e-9:
        return False, "entregou %.4f mL de 0,5 (limite %.4f)" % (delivered, bound)
    if w.get("rc") != "0" or w.get("state") != "idle":
        return False, "não voltou a idle depois do reconhecimento"
    return True, "parou em %.4f mL de 0,5, motor em repouso, alarme médio" % delivered


def _check_alarm(out, alarm, limit, then="paused", persists=False):
    v = _kv(out)
    elapsed = _elapsed(v)
    if elapsed is None:
        return False, "sem os instantes"
    if alarm not in v.get("alarms", ""):
        return False, "alarme esperado %s, visto %s" % (alarm, v.get("alarms"))
    if v.get("motor") != "0":
        return False, "alarme com o motor acionado"
    if elapsed > limit:
        return False, "alarme em %.2f s, limite %.1f s" % (elapsed, limit)
    if persists and v.get("rc_persist") != "1":
        return False, "reconhecido com a causa ainda presente"
    if v.get("rc") != "0" or v.get("state") != then:
        return False, "depois do reconhecimento: rc=%s state=%s (esperado %s)" % (
            v.get("rc"), v.get("state"), then)
    return True, "%s em %.2f s, motor em repouso, %s depois do reconhecimento" % (alarm, elapsed, then)


def check_occlusion(out):
    return _check_alarm(out, "occlusion:high", 30.0, persists=True)


def check_air(out):
    return _check_alarm(out, "air:high", 1.0)


def check_delivery_fault(out):
    return _check_alarm(out, "delivery_fault:high", 2.6)


def check_producer_lost(out):
    v = _kv(out)
    ok, detail = _check_alarm(out.replace("motor=1", "motor=0"), "mechanism_lost:high", 1.0)
    # A hung producer is a producer whose motor nobody can stop from here: the
    # controller cannot see it at rest, and the assertion does not ask it to.
    return ok, detail.replace("motor em repouso, ", "") + " (motor=%s, visto pelo controlador)" % v.get("motor")


def check_deadman(out):
    m = re.search(r"dead-man tripped (\d+) ms after the last message \(window (\d+) ms\)", out or "")
    v = _kv(out)
    if not m:
        return False, "o mecanismo não registrou o disparo do homem-morto"
    silent, window = int(m.group(1)), int(m.group(2))
    if v.get("trips") != "1":
        return False, "%s disparos, esperado 1" % v.get("trips")
    if not window < silent <= window + 50:
        return False, "disparou %d ms depois do último sinal (janela %d)" % (silent, window)
    if v.get("seen") != "alarm" or v.get("motor") != "0":
        return False, "o controlador não viu o disparo: state=%s motor=%s" % (v.get("seen"), v.get("motor"))
    if v.get("rc") != "0" or v.get("state") != "paused":
        return False, "depois do reconhecimento: rc=%s state=%s" % (v.get("rc"), v.get("state"))
    return True, "motor parado %d ms depois do último sinal (janela %d), com o controlador congelado" % (silent, window)


def check_no_auto_resume(out):
    v = _kv(out)
    if v.get("nr") != "1" or v.get("active") != "active":
        return False, "o serviço não reiniciou uma vez (nr=%s, %s)" % (v.get("nr"), v.get("active"))
    if v.get("state") != "alarm" or "therapy_interrupted:high" not in v.get("alarms", ""):
        return False, "voltou %s com %s, não em alarme" % (v.get("state"), v.get("alarms"))
    if v.get("motor") != "0" or v.get("therapy_active") != "0":
        return False, "voltou com o motor acionado ou a terapia ativa"
    if v.get("rc") != "0" or v.get("final") != "idle":
        return False, "depois do reconhecimento: rc=%s %s" % (v.get("rc"), v.get("final"))
    return True, "reiniciado com terapia em curso: voltou parado e em alarme; ocioso só depois do reconhecimento"


def check_alarm_silence(out):
    v = _kv(out)
    mid = _kv(_after(out, "@@mid").split("@@end", 1)[0])
    end = _kv(_after(out, "@@end"))
    if v.get("rc") != "0" or mid.get("silenced") != "1":
        return False, "o silêncio não foi aceito, ou não durou 30 s"
    if end.get("silenced") != "0" or "air:high" not in end.get("alarms", ""):
        return False, "o alarme não voltou depois de 60 s: %s" % end
    if end.get("expired") != "1":
        return False, "o fim do silêncio não está na trilha (%s)" % end.get("expired")
    return True, "silenciado; ainda silenciado a 30 s; o alarme voltou a 62 s, com registro"


def check_audit_trail(out):
    v = _kv(out)
    m = re.search(r"entries=(\d+) components=(\d+) breaks=(\d+)", out or "")
    if not m or v.get("rc") != "0" or m.group(3) != "0":
        first = re.search(r"^break .*$", out or "", re.M)
        return False, "a cadeia do controlador não verifica: %s" % (first.group(0) if first else "?")
    wanted = ["THERAPY_STARTED", "THERAPY_STOPPED", "ALARM_RAISED", "ALARM_ACKNOWLEDGED",
              "SAFETY_LIMIT_VIOLATION", "SAFETY_LIMIT_OVERRIDDEN", "ACTUATOR_FAULT",
              "CONFIGURATION_CHANGED", "SELF_TEST_PASSED"]
    missing = [e for e in wanted if int(v.get(e, "0") or 0) == 0]
    if missing:
        return False, "eventos ausentes da trilha: %s" % ", ".join(missing)
    return True, "%s registros, cadeia íntegra; os %d tipos de evento da terapia presentes" % (
        m.group(1), len(wanted))


# The infusion pump profile (implementation_plan_infusion_pump.md). The first
# four (phase 6) say the image comes up with the whole profile running; the
# rest (phase 7) drive one hazard's control each, with its fault injected
# through the simulator's fault port, and see it act. update-blocked (D7) is
# not here: the decision it depends on was not taken.
INFUSION_CHECKS = [
    ("infusion-active", "o serviço de controle está rodando, sem reinícios",
     "systemctl show infusion-control.service -p ActiveState -p NRestarts",
     rx(r"ActiveState=active[\s\S]*NRestarts=0|NRestarts=0[\s\S]*ActiveState=active")),

    ("infusion-sim-active", "o mecanismo simulado está rodando, sem reinícios",
     "systemctl show infusion-sim.service -p ActiveState -p NRestarts",
     rx(r"ActiveState=active[\s\S]*NRestarts=0|NRestarts=0[\s\S]*ActiveState=active")),

    ("drug-library-seal", "a biblioteca de fármacos casa com o seu selo",
     'test "$(sha256sum /etc/medplatform/infusion.conf | cut -d" " -f1)" '
     '= "$(cat /etc/medplatform/infusion.conf.sha256)" && echo SEAL_OK || echo SEAL_BAD',
     rx(r"SEAL_OK")),

    ("infusion-idle", "o controlador responde, ocioso e sem alarme",
     "infusion-ctl status; echo rc=$?", rx(r"idle none[\s\S]*rc=0")),

    ('ders-hard-limit', 'uma taxa acima do limite rígido é recusada e auditada (INF-01)',
     'B=$(journalctl -q MED_AUDIT_EVENT=SAFETY_LIMIT_VIOLATION|wc -l); infusion-ctl program drug=heparin syringe=50ml rate_ml_h=60 vtbi_ml=5 confirm=1 operator=suite; echo rc=$?; sleep 1; A=$(journalctl -q MED_AUDIT_EVENT=SAFETY_LIMIT_VIOLATION|wc -l); echo audit=$((A-B)); grep ^state= /run/medplatform/infusion-status',
     check_ders_hard),

    ('ders-soft-limit', 'acima do limite flexível só com a confirmação de um operador, auditada (INF-02)',
     'S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=heparin syringe=50ml rate_ml_h=40 vtbi_ml=5; echo rc1=$?; B=$(journalctl -q MED_AUDIT_EVENT=SAFETY_LIMIT_OVERRIDDEN MED_OPERATOR=suite|wc -l); $I program drug=heparin syringe=50ml rate_ml_h=40 vtbi_ml=5 confirm=1 operator=suite; echo rc2=$?; sleep 1; A=$(journalctl -q MED_AUDIT_EVENT=SAFETY_LIMIT_OVERRIDDEN MED_OPERATOR=suite|wc -l); echo audit=$((A-B)); grep ^state= $S; $I stop >/dev/null',
     check_ders_soft),

    # Timed by the controller's own records, not by the shell: the shell's
    # clock brackets two launches of infusion-ctl, 0.6 s on QEMU, and the first
    # run of this assertion measured that instead of the motor (phase 7 record).
    # THERAPY_STARTED and THERAPY_STOPPED are written after the mechanism has
    # acknowledged the rate and the rest, so they bracket the commanded flow.
    ('delivery-volume', 'o volume entregue pelo encoder é a integral da taxa comandada, a 3% (INF-04)',
     'S=/run/medplatform/infusion-status; I=infusion-ctl; '
     '$I program drug=saline syringe=50ml rate_ml_h=360 vtbi_ml=20 >/dev/null; '
     '$I start >/dev/null; sleep 10; $I pause >/dev/null; sleep 0.5; '
     'for e in THERAPY_STARTED THERAPY_STOPPED; do journalctl -q -n1 MED_AUDIT_EVENT=$e '
     'MED_COMPONENT=infusion-control -o verbose | sed -n "s/^ *MED_AUDIT_TIME=/$e=/p"; done; '
     "grep -E '^(delivered_ml|state)=' $S; $I stop >/dev/null",
     check_delivery_volume),

    ('vtbi-complete', 'ao fim do VTBI a bomba para e alarma, sem passar dele (INF-08)',
     "S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=saline syringe=50ml rate_ml_h=400 vtbi_ml=0.5 >/dev/null; $I start >/dev/null; sleep 6; grep -E '^(state|alarms|delivered_ml|motor)=' $S; echo @@end; $I acknowledge operator=suite >/dev/null; echo rc=$?; $I stop >/dev/null; grep ^state= $S",
     check_vtbi),

    ('occlusion-alarm', 'uma oclusão injetada alarma em alta prioridade e para o motor (INF-06)',
     "S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=saline syringe=50ml rate_ml_h=400 vtbi_ml=20 >/dev/null; $I start >/dev/null; sleep 3; $F occlusion 1; t0=$(cut -d' ' -f1 /proc/uptime); i=0; while ! grep -q '^state=alarm' $S && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done; t1=$(cut -d' ' -f1 /proc/uptime); echo t0=$t0 t1=$t1; sleep 1; grep -E '^(alarms|motor)=' $S; $I acknowledge operator=suite >/dev/null; echo rc_persist=$?; $F occlusion 0; sleep 3; $I acknowledge operator=suite >/dev/null; echo rc=$?; grep ^state= $S; $I stop >/dev/null; $I acknowledge operator=suite >/dev/null; true",
     check_occlusion),

    ('air-alarm', 'ar injetado na linha alarma e para o motor (INF-07)',
     "S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=saline syringe=50ml rate_ml_h=400 vtbi_ml=20 >/dev/null; $I start >/dev/null; sleep 1; $F air 1; t0=$(cut -d' ' -f1 /proc/uptime); i=0; while ! grep -q '^state=alarm' $S && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; t1=$(cut -d' ' -f1 /proc/uptime); echo t0=$t0 t1=$t1; sleep 0.5; grep -E '^(alarms|motor)=' $S; $F air 0; sleep 0.5; $I acknowledge operator=suite >/dev/null; echo rc=$?; grep ^state= $S; $I stop >/dev/null; $I acknowledge operator=suite >/dev/null; true",
     check_air),

    ('delivery-fault', 'um motor travado (comandado e não medido) alarma (INF-05)',
     "S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=saline syringe=50ml rate_ml_h=400 vtbi_ml=20 >/dev/null; $I start >/dev/null; sleep 1; $F stall 1; t0=$(cut -d' ' -f1 /proc/uptime); i=0; while ! grep -q '^state=alarm' $S && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; t1=$(cut -d' ' -f1 /proc/uptime); echo t0=$t0 t1=$t1; sleep 0.5; grep -E '^(alarms|motor)=' $S; $F stall 0; sleep 0.5; $I acknowledge operator=suite >/dev/null; echo rc=$?; grep ^state= $S; $I stop >/dev/null; $I acknowledge operator=suite >/dev/null; true",
     check_delivery_fault),

    ('alarm-silence', 'um alarme silenciado volta depois de T_sil se a causa persistir (INF-12)',
     "S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=saline syringe=50ml rate_ml_h=100 vtbi_ml=20 >/dev/null; $I start >/dev/null; sleep 1; $F air 1; sleep 1; B=$(journalctl -q MED_AUDIT_EVENT=ALARM_RAISED | grep -c 'silence expired'); $I silence operator=suite >/dev/null; echo rc=$?; sleep 30; echo @@mid; grep ^silenced= $S; sleep 32; echo @@end; grep -E '^(silenced|alarms)=' $S; echo expired=$(( $(journalctl -q MED_AUDIT_EVENT=ALARM_RAISED | grep -c 'silence expired') - B )); $F air 0; sleep 0.5; $I acknowledge operator=suite >/dev/null; grep ^state= $S; $I stop >/dev/null; $I acknowledge operator=suite >/dev/null; true",
     check_alarm_silence),

    ('producer-lost', 'um mecanismo que para de responder alarma no controlador (INF-10)',
     "S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=saline syringe=50ml rate_ml_h=100 vtbi_ml=20 >/dev/null; $I start >/dev/null; sleep 1; $F silent 1; t0=$(cut -d' ' -f1 /proc/uptime); i=0; while ! grep -q '^state=alarm' $S && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done; t1=$(cut -d' ' -f1 /proc/uptime); echo t0=$t0 t1=$t1; grep -E '^(alarms|motor)=' $S; $F silent 0; sleep 1; $I acknowledge operator=suite >/dev/null; echo rc=$?; grep ^state= $S; $I stop >/dev/null; $I acknowledge operator=suite >/dev/null; true",
     check_producer_lost),

    ('deadman', 'com o controlador congelado, o mecanismo para o motor sozinho (INF-09)',
     "S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=saline syringe=50ml rate_ml_h=100 vtbi_ml=20 >/dev/null; $I start >/dev/null; sleep 1; P=$(systemctl show -p MainPID --value infusion-control); B=$(journalctl -q -u infusion-sim | grep -c 'dead-man tripped'); kill -STOP $P; sleep 2; journalctl -q -u infusion-sim | grep 'dead-man tripped' | tail -n 1; echo trips=$(( $(journalctl -q -u infusion-sim | grep -c 'dead-man tripped') - B )); kill -CONT $P; sleep 2; echo seen=$(sed -n 's/^state=//p' $S) $(grep ^motor= $S); $I acknowledge operator=suite >/dev/null; echo rc=$?; grep ^state= $S; $I stop >/dev/null; $I acknowledge operator=suite >/dev/null; true",
     check_deadman),

    ('no-auto-resume', 'reiniciado durante a infusão, o serviço volta parado e em alarme (INF-11)',
     "S=/run/medplatform/infusion-status; F=infusion-sim-fault; I=infusion-ctl; $I program drug=saline syringe=50ml rate_ml_h=100 vtbi_ml=20 >/dev/null; $I start >/dev/null; sleep 1; B=$(systemctl show -p NRestarts --value infusion-control); systemctl kill -s KILL infusion-control; sleep 9; echo nr=$(( $(systemctl show -p NRestarts --value infusion-control) - B )) active=$(systemctl is-active infusion-control); grep -E '^(state|alarms|motor|therapy_active)=' $S; $I acknowledge operator=suite >/dev/null; echo rc=$?; echo final=$(sed -n 's/^state=//p' $S)",
     check_no_auto_resume),

    ('audit-trail', 'cada transição e alarme está na trilha, e a cadeia verifica (INF-13)',
     'journalctl -o export MED_AUDIT=1 MED_COMPONENT=infusion-control > /run/infusion-audit.export; med-audit-verify /run/infusion-audit.export; echo rc=$?; rm -f /run/infusion-audit.export; for e in THERAPY_STARTED THERAPY_STOPPED ALARM_RAISED ALARM_ACKNOWLEDGED SAFETY_LIMIT_VIOLATION SAFETY_LIMIT_OVERRIDDEN ACTUATOR_FAULT CONFIGURATION_CHANGED SELF_TEST_PASSED; do echo $e=$(journalctl -q MED_AUDIT_EVENT=$e MED_COMPONENT=infusion-control | wc -l); done',
     check_audit_trail),
]


PROFILES = {
    "eeg": ("make runqemu", PLATFORM_CHECKS + EEG_CHECKS),
    "tomograph": ("make runqemu-tomograph", PLATFORM_CHECKS),
    # The second device class: the platform's assertions unchanged (metric M7
    # of implementation_plan_infusion_pump.md), and its own (phase 7).
    "infusion": ("make runqemu-infusion", PLATFORM_CHECKS + INFUSION_CHECKS),
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

    # Uma linha de comando que o shell do alvo não aceita inteira é um
    # defeito da suíte, e é melhor dito aqui, em um segundo, do que depois de
    # um boot e de um timeout de 90 s que não diz por quê. O limite do editor
    # de linha do alvo é 1024; os marcadores que run() acrescenta levam ~80.
    too_long = [(cid, len(command)) for cid, _, command, _ in checks
                if not callable(command) and len(command) > 900]
    if too_long:
        print("FALHA: comando longo demais para o shell do alvo (limite 900): " +
              ", ".join(f"{cid} ({n})" for cid, n in too_long), file=sys.stderr)
        return 2

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
            # A command built when it runs, from what an earlier check learnt
            # (journal-sealed needs the key journal-seal-key collected).
            if callable(command):
                command = command()
                if len(command) > 900:
                    out = None
                    results.append((cid, claim, False, "comando longo demais para o shell do alvo"))
                    print(f"  FAIL  {cid:<24} {claim}")
                    continue
            out = con.run(cid, command)
            if out is None:
                ok, detail = False, "sem resposta do guest"
            else:
                ok, detail = predicate(out)
            results.append((cid, claim, ok, detail))
            print(f"  {'PASS' if ok else 'FAIL'}  {cid:<24} {claim}")
            # MEDCHECK_VERBOSE=1 prints what a passing check saw, too: the
            # number a RESULTS.md entry quotes, not just the verdict.
            if not ok or os.environ.get("MEDCHECK_VERBOSE") == "1":
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
