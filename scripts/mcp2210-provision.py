#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Provisionamento da NVRAM do MCP2210 para a pinagem da placa de AFE.
#
# ISTO ESCREVE NA NVRAM. Existe porque a configuracao de fabrica briga com a
# placa (BRINGUP_AFE.md §2.3): GP1, GP2 e GP3 vao ao terra e, de fabrica, sao
# saidas que ficam em nivel alto; GP5 recebe o DRDY do conversor e, de fabrica,
# e a saida USBCFG. A NVRAM e carregada no reset, entao so ela torna o
# power-up seguro - nenhum driver chega antes.
#
# Por padrao so MOSTRA o que gravaria. Gravar exige --write E digitar a
# palavra de confirmacao num terminal. Roda no host, com a ponte presa ao
# hid-generic (antes do hid-mcp2210), como o snapshot.
#
#   sudo python3 scripts/mcp2210-provision.py            # mostra, nao grava
#   sudo python3 scripts/mcp2210-provision.py --write    # grava, depois de confirmar
#
# O que este programa NUNCA faz, por construcao:
#   - escrever qualquer bloco que nao seja chip settings (0x60/0x20): SPI,
#     VID/PID e strings ficam intocados, e isso e verificado na releitura;
#   - escrever controle de acesso diferente de 0x00. 0x80 trava a NVRAM para
#     sempre, numa peca soldada; 0x40 poe uma senha. Nenhum dos dois tem
#     caminho de volta que este projeto queira testar;
#   - enviar senha (0x70), ou gravar numa peca que ja esteja protegida.
#
# Tabela 3-1 (Set Chip Settings Power-up Default) e 3-20 (a leitura), na
# transcricao docs/Register_Map_MCP2210.md.

import importlib.util
import os
import select
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "snap", os.path.join(HERE, "mcp2210-nvram-snapshot.py"))
snap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(snap)

SET_NVRAM = 0x60
SUB_CHIP = 0x20
REPORT = snap.REPORT
CONFIRM = "GRAVAR"

GPIO, CS, DEDICATED = 0x00, 0x01, 0x02

# A placa, pino a pino. (designacao, direcao de power-up, nivel de power-up,
# papel na placa). Direcao: True = entrada. O nivel so vale para saidas, e
# todo pino que nao e o LED nasce com saida padrao 0: um pino aterrado que
# alguem mude para saida dirige 0 contra o terra, nao 1. O LED nasce aceso, como
# indicador de que a ponte esta alimentada e configurada; ativo em nivel alto,
# verificado na bancada (GP6 = 1 acende).
BOARD = [
    (GPIO, True,  0, "GND na placa"),
    (GPIO, True,  0, "GND na placa"),
    (GPIO, True,  0, "GND na placa"),
    (GPIO, True,  0, "GND na placa"),
    (CS,   True,  0, "CS do conversor (o modulo SPI controla o pino)"),
    (GPIO, True,  0, "DRDY do conversor - entrada"),
    (GPIO, False, 1, "LED - saida, nasce aceso"),
    (GPIO, True,  0, "GND na placa"),
    (GPIO, True,  0, "GND na placa (GP8 so pode ser entrada)"),
]

NAMES = {GPIO: "GPIO", CS: "chip select", DEDICATED: "dedicado"}


def die(msg):
    print(f"ERRO: {msg}", file=sys.stderr)
    sys.exit(2)


def target_block(current):
    """O bloco de chip settings a gravar, montado sobre a leitura atual."""
    t = bytearray(current)
    for gp, (desig, _, _, _) in enumerate(BOARD):
        t[4 + gp] = desig
    direction = level = 0
    for gp, (_, is_input, high, _) in enumerate(BOARD[:8]):
        if is_input and high:
            raise AssertionError(f"GP{gp}: nivel alto declarado numa entrada")
        direction |= int(is_input) << gp
        level |= high << gp
    t[13] = level                 # saida padrao GP7..GP0: so o LED alto
    t[15] = direction             # direcao GP7..GP0
    t[16] = current[16] | 0x01    # bit 0 = GP8, so entrada (Tab. 3-1, nota no campo)
    # byte 14 (alto da saida, don't care) e 17 (outros: wake-up, modo de
    # interrupcao, liberacao do barramento) ficam como estao.
    t[18] = 0x00                  # sem protecao - e so isso
    return bytes(t)


def write_chip_settings(fd, block):
    tx = bytearray(REPORT)
    tx[0], tx[1] = SET_NVRAM, SUB_CHIP
    tx[4:19] = block[4:19]        # bytes 4..18 da Tabela 3-1
    # 19..26 nova senha: zero = "nao mudar" (nota 1). 27..63 reservado: zero.
    if tx[18] != 0x00 or any(tx[19:]):
        raise AssertionError("recusado: controle de acesso ou senha nao-zero")
    os.write(fd, b"\x00" + bytes(tx))
    r, _, _ = select.select([fd], [], [], 2.0)
    if not r:
        die("sem resposta ao 0x60/0x20 em 2 s - estado da NVRAM desconhecido; "
            "rode o snapshot antes de qualquer outra coisa")
    rx = os.read(fd, REPORT)
    if rx[0] != SET_NVRAM or rx[2] != SUB_CHIP:
        die(f"eco errado ao 0x60/0x20: {rx[:3].hex()}")
    if rx[1] != 0x00:
        die(f"o chip recusou a gravacao, estado 0x{rx[1]:02x} (protegido?)")


def show(current, target):
    print(f"  {'pino':4s}  {'hoje':12s}  {'depois':12s}  papel na placa")
    for gp, (desig, is_input, high, role) in enumerate(BOARD):
        now = NAMES.get(current[4 + gp], f"0x{current[4 + gp]:02x}")
        new = NAMES[desig]
        if desig == GPIO:
            new += " in" if is_input else f" out={high}"
        mark = "*" if current[4 + gp] != desig else " "
        print(f"{mark} GP{gp}   {now:12s}  {new:12s}  {role}")
    for idx, name in ((13, "saida padrao"), (15, "direcao"), (16, "direcao GP8"),
                      (17, "outros"), (18, "acesso")):
        mark = "*" if current[idx] != target[idx] else " "
        print(f"{mark} {name:13s} 0x{current[idx]:02x} -> 0x{target[idx]:02x}")


def main():
    write = "--write" in sys.argv[1:]

    dev, usb, before = snap.snapshot()          # recusa se nao for hid-generic
    cur = before[SUB_CHIP]
    if cur[18] != 0x00:
        die(f"a NVRAM ja esta protegida (acesso 0x{cur[18]:02x}); este programa "
            "nao envia senha e nao destrava nada")

    target = target_block(cur)
    print(f"# MCP2210 em {dev}, serial {snap.sysfs(usb, 'serial')}\n")
    show(cur, target)

    if snap.documented_bytes(SUB_CHIP, cur) == snap.documented_bytes(SUB_CHIP, target):
        print("\n  A NVRAM ja esta no estado da placa. Nada a gravar.")
        return 0

    if not write:
        print("\n  Nada foi gravado. Para gravar: --write")
        return 0

    if not sys.stdin.isatty():
        die("a confirmacao precisa de um terminal")
    print(f"\n  Isto grava a NVRAM (linhas com *). Digite {CONFIRM} para continuar:")
    if input("  > ").strip() != CONFIRM:
        print("  Cancelado. Nada foi gravado.")
        return 1

    fd = os.open(dev, os.O_RDWR)
    try:
        while select.select([fd], [], [], 0)[0]:
            os.read(fd, REPORT)
        write_chip_settings(fd, target)
    finally:
        os.close(fd)

    # Releitura: o bloco gravado tem de ser o pedido, e os outros quatro
    # tem de estar exatamente como antes - o que prova que so um foi tocado.
    _, _, after = snap.snapshot()
    ok = True
    for sub in snap.SUBCMDS:
        want = target if sub == SUB_CHIP else before[sub]
        same = snap.documented_bytes(sub, after[sub]) == snap.documented_bytes(sub, want)
        ok &= same
        print(f"  {'PASS' if same else 'FAIL'}  0x{sub:02x} {snap.SUBCMDS[sub]}")
    if not ok:
        die("a releitura nao confere - NAO replugue antes de olhar o snapshot")
    print("\n  Gravado e conferido. A NVRAM so vale no proximo reset: desplugue e")
    print("  plugue a ponte, e tire um snapshot novo como linha de base:")
    print("    sudo python3 scripts/mcp2210-nvram-snapshot.py -o ~/mcp2210-nvram-provisionada.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
