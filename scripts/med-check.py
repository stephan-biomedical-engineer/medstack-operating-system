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
    which the command never passed, so a sealed file reports "Required key not
    available" rather than validating. What is measured here is structural
    integrity. Sealing is asserted by RESULTS.md §5 on other evidence.
    """
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    passed = [l for l in lines if l.startswith("PASS:")]
    failed = [l for l in lines if l.startswith("FAIL:")]
    archived_failed = [l for l in failed if not l.split()[1].endswith("/system.journal")]

    if archived_failed:
        return False, "journal arquivado não verifica: " + "; ".join(archived_failed)
    if not passed:
        return False, "nenhum journal verificou (saída: %s)" % (lines or "(vazia)")
    return True, "%d arquivo(s) verificam, %d ativo(s) ignorado(s)" % (
        len(passed), len(failed))


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

    # Uma linha de comando que o shell do alvo não aceita inteira é um
    # defeito da suíte, e é melhor dito aqui, em um segundo, do que depois de
    # um boot e de um timeout de 90 s que não diz por quê. O limite do editor
    # de linha do alvo é 1024; os marcadores que run() acrescenta levam ~80.
    too_long = [(cid, len(command)) for cid, _, command, _ in checks if len(command) > 900]
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
