#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Snapshot da NVRAM do MCP2210, por /dev/hidraw, SOMENTE LEITURA.
#
# Roda no HOST (o PC de desenvolvimento), com a ponte presa ao hid-generic -
# isto e, ANTES de carregar o hid-mcp2210. E a linha de base do teste negativo
# da Fase 8-B ("a aquisicao nunca escreve NVRAM"), e alcanca o que o
# afe-phase8b.sh na placa nao alcanca: os cinco blocos de power-up (0x61), nao
# so os descritores USB que eles produzem.
#
#   sudo python3 scripts/mcp2210-nvram-snapshot.py -o antes.txt
#   ... sessao ...
#   sudo python3 scripts/mcp2210-nvram-snapshot.py --compare antes.txt
#
# O veredito do --compare e sobre os bytes que as tabelas DOCUMENTAM; os
# "Don't care" sao gravados e diferencas neles sao informadas, nunca contadas.
#
# A unica coisa que este programa envia ao chip e 0x61 (Get NVRAM Settings)
# com um dos cinco sub-comandos abaixo. Isso nao e convencao: send() recusa
# qualquer outro par antes de abrir o arquivo para escrita. 0x60 (Set NVRAM)
# e 0x70 (senha) nao aparecem neste arquivo.
#
# Tabelas citadas sao do DS20005176, na transcricao docs/Register_Map_MCP2210.md.

import argparse
import glob
import hashlib
import os
import select
import sys

VID, PID = 0x04D8, 0x00DE
GET_NVRAM = 0x61
REPORT = 64

# sub-comando -> nome; o conjunto inteiro do que pode ser enviado
SUBCMDS = {
    0x10: "spi-power-up",      # transfer settings de power-up
    0x20: "chip-power-up",     # chip settings de power-up (Tabela 3-20)
    0x30: "usb-key",           # VID/PID/energia (Tabela 3-22)
    0x40: "product",           # string do produto (Tabela 3-26)
    0x50: "manufacturer",      # string do fabricante (Tabela 3-24)
}

# Os bytes que a tabela DOCUMENTA, por sub-comando. O resto e "Don't care", e
# na peca observada (2026-10-03) o fim das respostas 0x10 e 0x20 e o mesmo
# residuo de buffer a partir do byte 21 - nada garante que ele fique igual
# depois de outros comandos. Comparar esses bytes faria o teste negativo
# acusar uma escrita que nao houve. As strings sao tratadas a parte.
DOCUMENTED = {
    0x10: range(4, 21),        # bit rate .. SPI mode, bytes 4-20; 21-63 don't care
    0x20: range(4, 19),        # GP0..GP8, saidas, direcoes, outros, acesso; 19-63 don't care
    0x30: (12, 13, 14, 15, 29, 30),
}


def documented_bytes(sub, rx):
    if sub in DOCUMENTED:
        return bytes(rx[i] for i in DOCUMENTED[sub])
    n = rx[4]  # strings: comprimento, 0x03 e os 2*chars que o comprimento cobre
    return bytes(rx[4:4 + max(2, min(n, REPORT - 4))])


def die(msg):
    print(f"ERRO: {msg}", file=sys.stderr)
    sys.exit(2)


def find_bridge():
    """O hidraw da ponte, achado pelo HID_ID - nunca pelo numero, que e corrida."""
    want = f"HID_ID=0003:{VID:08X}:{PID:08X}"
    hits = []
    for node in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            with open(os.path.join(node, "device", "uevent")) as f:
                if want in f.read().split("\n"):
                    hits.append(node)
        except OSError:
            continue
    if not hits:
        # Um driver que nao expoe hidraw (o hid-mcp2210, autocarregado no
        # plug) produz o mesmo sintoma que a ponte desplugada.
        held = sorted({os.path.basename(os.path.realpath(os.path.join(d, "driver")))
                       for d in glob.glob(f"/sys/bus/hid/devices/0003:{VID:04X}:{PID:04X}.*")
                       if os.path.exists(os.path.join(d, "driver"))})
        if held:
            die(f"a ponte esta plugada, mas presa ao driver {', '.join(held)}, que nao "
                "expoe hidraw. Descarregue-o (sudo rmmod hid_mcp2210) e rode de novo")
        die(f"nenhum hidraw com {want}. A ponte esta plugada?")
    if len(hits) > 1:
        die(f"{len(hits)} pontes encontradas; este programa so sabe falar com uma")
    node = hits[0]
    drv = os.path.basename(os.path.realpath(os.path.join(node, "device", "driver")))
    if drv != "hid-generic":
        # Com o hid-mcp2210 preso, ele tambem fala com o chip e as respostas
        # se cruzam. O snapshot e para antes do driver, por desenho.
        die(f"a ponte esta presa a '{drv}', nao ao hid-generic. "
            "Rode isto antes de carregar o hid-mcp2210 (ou: sudo rmmod hid_mcp2210).")
    # hidraw -> dispositivo HID -> interface USB (1-3:1.0) -> dispositivo USB
    # (1-3). Os descritores moram no ultimo; sobe ate achar idVendor em vez de
    # contar niveis, que foi o defeito da primeira versao.
    usb = os.path.realpath(os.path.join(node, "device"))
    while usb != "/" and not os.path.exists(os.path.join(usb, "idVendor")):
        usb = os.path.dirname(usb)
    if usb == "/":
        die("nao achei o dispositivo USB acima do hidraw")
    return "/dev/" + os.path.basename(node), usb


def sysfs(path, name):
    try:
        with open(os.path.join(path, name)) as f:
            return f.read().strip()
    except OSError:
        return None


def send(fd, cmd, sub):
    if cmd != GET_NVRAM or sub not in SUBCMDS:
        raise AssertionError(f"recusado: 0x{cmd:02x}/0x{sub:02x} nao e leitura")
    tx = bytearray(REPORT)
    tx[0], tx[1] = cmd, sub
    os.write(fd, b"\x00" + bytes(tx))  # report ID 0 na frente
    r, _, _ = select.select([fd], [], [], 1.0)
    if not r:
        die(f"sem resposta a 0x{cmd:02x}/0x{sub:02x} em 1 s")
    rx = os.read(fd, REPORT)
    if len(rx) != REPORT:
        die(f"resposta de {len(rx)} bytes a 0x{sub:02x}, esperava {REPORT}")
    # Eco do comando no byte 0, estado no 1, eco do sub-comando no 2:
    # o mesmo casamento que o driver faz em mcp2210_raw_event().
    if rx[0] != cmd or rx[2] != sub:
        die(f"eco errado a 0x{cmd:02x}/0x{sub:02x}: {rx[:3].hex()}")
    if rx[1] != 0x00:
        die(f"estado 0x{rx[1]:02x} a 0x{sub:02x} (esperava 0x00, sucesso)")
    return rx


def usb_string(rx):
    n = rx[4]  # comprimento do descritor: 2*chars + 2 (Tabelas 3-24/3-26)
    if rx[5] != 0x03 or n < 2 or n > REPORT - 4:
        return None
    return rx[6:6 + n - 2].decode("utf-16-le", errors="replace")


def snapshot():
    dev, usb = find_bridge()
    fd = os.open(dev, os.O_RDWR)
    try:
        # descarta relatorio pendente de alguem antes de nos
        while select.select([fd], [], [], 0)[0]:
            os.read(fd, REPORT)
        raw = {sub: send(fd, GET_NVRAM, sub) for sub in SUBCMDS}
    finally:
        os.close(fd)
    return dev, usb, raw


def report(dev, usb, raw):
    lines = []
    lines.append(f"# MCP2210 NVRAM snapshot, {dev}, USB {os.path.basename(usb)}")
    lines.append(f"# serial {sysfs(usb, 'serial')}, bcdDevice {sysfs(usb, 'bcdDevice')}")
    for sub, name in SUBCMDS.items():
        lines.append(f"0x{sub:02x} {name:13s} {raw[sub].hex()}")
    for sub, name in SUBCMDS.items():
        lines.append(f"doc  0x{sub:02x} {name:13s} {documented_bytes(sub, raw[sub]).hex()}")
    digest = hashlib.sha256(b"".join(documented_bytes(s, raw[s]) for s in SUBCMDS)).hexdigest()
    lines.append(f"sha256-documented {digest}")

    # Decodificacao, e cada campo conferido contra a enumeracao USB, que e uma
    # fonte independente da tabela: se o deslocamento estiver errado, discorda.
    k = raw[0x30]
    nv_vid = k[12] | k[13] << 8
    nv_pid = k[14] | k[15] << 8
    checks = [
        ("VID (0x30 bytes 12-13)", f"{nv_vid:04x}", sysfs(usb, "idVendor")),
        ("PID (0x30 bytes 14-15)", f"{nv_pid:04x}", sysfs(usb, "idProduct")),
        ("produto (0x40)", usb_string(raw[0x40]), sysfs(usb, "product")),
        ("fabricante (0x50)", usb_string(raw[0x50]), sysfs(usb, "manufacturer")),
    ]
    ok = True
    print("\n".join(lines))
    print()
    for what, nv, enum in checks:
        same = nv == enum
        ok &= same
        print(f"  {'PASS' if same else 'FAIL'}  {what}: NVRAM={nv!r} enumeracao={enum!r}")
    print(f"  info  energia (0x30 byte 29) 0x{k[29]:02x}, "
          f"corrente (byte 30) {k[30] * 2} mA")
    return lines, ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1] if __doc__ else None)
    ap.add_argument("-o", "--output", help="grava o snapshot neste arquivo")
    ap.add_argument("--compare", help="compara com um snapshot anterior, byte a byte")
    a = ap.parse_args()

    dev, usb, raw_now = snapshot()
    lines, ok = report(dev, usb, raw_now)

    if a.output:
        with open(a.output, "w") as f:
            f.write("\n".join(lines) + "\n")
        print(f"\n  gravado em {a.output}")

    if a.compare:
        with open(a.compare) as f:
            old = f.read().splitlines()
        # O veredito e sobre os campos documentados. Um snapshot da versao
        # anterior nao tem linhas "doc": entao os campos sao recalculados
        # do hex bruto dele, que e o mesmo dado.
        before = {}
        for l in old:
            p = l.split()
            if len(p) == 3 and p[0].startswith("0x"):
                sub = int(p[0], 16)
                before[sub] = bytes.fromhex(p[2])
        if set(before) != set(SUBCMDS):
            die(f"{a.compare} nao tem os cinco blocos")
        changed = [s for s in SUBCMDS
                   if documented_bytes(s, before[s]) != documented_bytes(s, raw_now[s])]
        if not changed:
            print(f"\n  PASS  campos documentados da NVRAM identicos a {a.compare}")
        else:
            ok = False
            print(f"\n  FAIL  NVRAM MUDOU em relacao a {a.compare}:")
            for s in changed:
                print(f"    0x{s:02x} antes: {documented_bytes(s, before[s]).hex()}")
                print(f"    0x{s:02x} agora: {documented_bytes(s, raw_now[s]).hex()}")
        # Informativo, nunca veredito: os bytes "don't care" que mudaram.
        for s in SUBCMDS:
            if before[s] != raw_now[s] and s not in changed:
                diff = [i for i in range(REPORT) if before[s][i] != raw_now[s][i]]
                print(f"  info  0x{s:02x}: {len(diff)} byte(s) don't-care diferentes "
                      f"(indices {diff[0]}..{diff[-1]}); nao contam")

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
