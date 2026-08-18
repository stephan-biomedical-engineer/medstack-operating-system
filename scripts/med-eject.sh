#!/bin/sh
#
# MedPlatform: desmonta e desliga com segurança o disco removível que guarda
# downloads/ e sstate-cache/.
#
# POR QUE ISTO EXISTE, E POR QUE NÃO É SÓ CONVENIÊNCIA
# O disco de caches e o cartão SD do alvo aparecem como o mesmo tipo de
# dispositivo, com letras vizinhas (/dev/sdb e /dev/sdc). Gravar uma imagem no
# dispositivo errado destrói o cache inteiro - e "bmaptool copy ... /dev/sdb" é
# um erro de uma tecla. Retirar o disco antes de gravar não *reduz* esse risco,
# ele o elimina: o alvo errado deixa de existir.
#
# Este script não força nada. Se alguma coisa estiver usando o disco, ele diz o
# que é, com PID e nome do processo, e sai com código não-zero - porque o modo
# de falha de um "umount -l" é perder escrita em voo, silenciosamente, num disco
# que guarda 21 GB de trabalho.
#
# ONDE ESTE ARQUIVO SABE COISAS, E ONDE NÃO SABE
# Não há UUID nem /dev/sdX escrito aqui. O ponto de montagem vem do argumento,
# de MED_HD_MOUNT, ou do padrão ~/hd, e o dispositivo é resolvido a partir dele.
# O mesmo motivo pelo qual nenhuma camada nomeia placa: o caminho é fato da
# máquina, e fatos da máquina ficam em local.mk (gitignored) ou no ambiente.
#
# USO
#   scripts/med-eject.sh [ponto-de-montagem]
#   MED_HD_MOUNT=/mnt/cache scripts/med-eject.sh
#
# SAÍDA
#   0  desmontado e desligado, ou já não estava montado
#   1  em uso - nada foi feito
#   2  erro ao desmontar ou desligar

set -eu

MOUNT=${1:-${MED_HD_MOUNT:-$HOME/hd}}

say()  { printf '%s\n' "$*"; }
fail() { printf '%s\n' "$*" >&2; exit "${2:-2}"; }

# ---------------------------------------------------------------------------
# Já desmontado? Então não há nada a fazer, e isso não é erro: o script tem de
# ser idempotente para poder virar hábito antes de cada gravação.
# ---------------------------------------------------------------------------
if ! mountpoint -q "$MOUNT" 2>/dev/null; then
    say "nada a fazer: $MOUNT não está montado"
    exit 0
fi

DEV=$(findmnt -n -o SOURCE --target "$MOUNT" | head -1)
[ -n "$DEV" ] || fail "não consegui descobrir o dispositivo por trás de $MOUNT"

# O power-off atua no disco inteiro, não na partição: /dev/sdb1 -> /dev/sdb.
PARENT=$(lsblk -no PKNAME "$DEV" 2>/dev/null | head -1)
[ -n "$PARENT" ] && DISK=/dev/$PARENT || DISK=$DEV

say "disco:  $DEV (${DISK})"
say "montado em:"
findmnt -n -o TARGET --source "$DEV" | sed 's/^/  /'

# ---------------------------------------------------------------------------
# Quem está usando? Duas perguntas diferentes, e as duas importam.
#
# Um build em andamento é motivo suficiente por si só, mesmo que naquele
# instante nenhum arquivo esteja aberto: o bitbake abre e fecha milhares de
# arquivos do sstate ao longo de uma execução, então "nada aberto agora" não
# significa "seguro daqui a um segundo".
#
# E o caso mais comum na prática não é arquivo aberto, é um shell com o cwd lá
# dentro - foi exatamente o que impediu o primeiro umount desta máquina.
# ---------------------------------------------------------------------------
BUSY=0

if pgrep -x bitbake >/dev/null 2>&1 || pgrep -f "bitbake-worker|kas-container" >/dev/null 2>&1; then
    say ""
    say "RECUSANDO: há build em andamento"
    pgrep -af "bitbake|kas-container" | sed 's/^/  /'
    BUSY=1
fi

HOLDERS=$(
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        comm=$(tr -d '\0' < "$p/comm" 2>/dev/null || true)
        cwd=$(readlink "$p/cwd" 2>/dev/null || true)
        case "$cwd" in
            "$MOUNT"|"$MOUNT"/*) printf '  PID %s (%s) está com o diretório de trabalho em %s\n' "$pid" "$comm" "$cwd" ;;
        esac
        for fd in "$p"/fd/*; do
            t=$(readlink "$fd" 2>/dev/null || true)
            case "$t" in
                "$MOUNT"|"$MOUNT"/*) printf '  PID %s (%s) mantém aberto %s\n' "$pid" "$comm" "$t" ;;
            esac
        done
    done 2>/dev/null | sort -u
)

if [ -n "$HOLDERS" ]; then
    say ""
    say "RECUSANDO: processos usando $MOUNT"
    printf '%s\n' "$HOLDERS"
    say ""
    say "dica: um shell com 'cd' lá dentro basta para segurar. Saia com 'cd ~'."
    BUSY=1
fi

[ "$BUSY" -eq 0 ] || exit 1

# ---------------------------------------------------------------------------
# Desmontar. sync primeiro por cinto e suspensório - o umount já força a
# descarga, mas se ele falhar por qualquer motivo, o sync já terá acontecido.
#
# Desmonta *todas* as montagens deste dispositivo, não só a pedida: este disco
# já apareceu montado duas vezes ao mesmo tempo (fstab em ~/hd e udisks em
# /media), e desmontar uma só deixaria a outra viva segurando o hardware.
# ---------------------------------------------------------------------------
say ""
say "sincronizando..."
sync

# Sem pipeline aqui de propósito: "findmnt | while read" põe o laço num
# subshell, e um fail lá dentro encerraria só o subshell - o script seguiria
# adiante como se tivesse desmontado. A checagem logo abaixo pegaria o caso do
# ponto de montagem pedido, mas não pegaria uma segunda montagem do mesmo
# dispositivo em outro lugar.
TARGETS=$(findmnt -n -o TARGET --source "$DEV")
for target in $TARGETS; do
    say "desmontando $target"
    umount "$target" 2>/dev/null || sudo umount "$target" \
        || fail "falha ao desmontar $target"
done

REMAINING=$(findmnt -n -o TARGET --source "$DEV" || true)
[ -z "$REMAINING" ] || fail "ainda montado em: $REMAINING"

if mountpoint -q "$MOUNT" 2>/dev/null; then
    fail "$MOUNT continua montado após o umount"
fi

# ---------------------------------------------------------------------------
# Desligar. Este é o passo que quase todo mundo pula, e é o que estaciona as
# cabeças e corta a alimentação da gaveta. Sem ele, "desmontado" apenas
# significa que o sistema de arquivos está consistente - o disco continua
# girando quando o cabo sai.
#
# Não é fatal se falhar: o sistema de arquivos já está a salvo nesse ponto.
# ---------------------------------------------------------------------------
say "desligando $DISK ..."
if udisksctl power-off -b "$DISK" >/dev/null 2>&1; then
    say ""
    say "pronto: $MOUNT desmontado e $DISK desligado. Pode desconectar o cabo."
else
    say ""
    say "AVISO: o umount funcionou, mas o power-off de $DISK falhou."
    say "O sistema de arquivos está consistente e desconectar é seguro; o disco"
    say "só não vai estacionar as cabeças sozinho. Alguns adaptadores USB não"
    say "suportam o comando."
fi

# ---------------------------------------------------------------------------
# E o motivo de tudo isto, dito na saída, porque o script existe para ser usado
# imediatamente antes de uma gravação.
# ---------------------------------------------------------------------------
say ""
say "confira antes de gravar: o dispositivo do cartão SD agora é o único"
say "removível presente. Compare lsblk antes e depois de inseri-lo."
