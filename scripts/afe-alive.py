#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# O conversor está vivo? Para quando o oráculo lê 0xff em todo registrador.
#
# Um 0xff constante só diz que nada puxa a MISO para baixo. Não separa "chip
# morto, sem alimentação ou sem clock" de "chip vivo cujo DOUT não chega à
# ponte". Este programa separa os dois pelo único fio que o conversor dirige e
# que não passa pela MISO: o DRDY, no GP5 desta placa (BRINGUP_AFE.md §2.3).
#
#   Antes do START não há conversão, e o DRDY fica ALTO.
#   Depois do START (comando 0x08, pela MOSI) a primeira conversão o baixa, e
#   sem leitura ele fica BAIXO, salvo um pulso de 4 tCLK (~2 us) por amostra -
#   curto demais para uma leitura de GPIO por HID ver.
#
# Então: DRDY alto -> baixo depois do START prova alimentação, clock, RESET e
# PWDN altos, e que CS, SCLK e MOSI chegam ao chip. O defeito fica no caminho
# do DOUT. DRDY parado não prova chip queimado: prova só que ele não converteu.
#
# Roda no HOST, com a ponte no hid-generic, como o oráculo:
#
#   sudo modprobe -r hid_mcp2210
#   sudo python3 scripts/afe-alive.py
#   sudo modprobe hid_mcp2210
#
# Da ponte, além do que o oráculo usa (0x40/0x41/0x42), só lê: 0x20 (chip
# settings, para conferir que o GP5 é entrada GPIO) e 0x31 (níveis dos pinos).
# Nunca escreve GPIO, NVRAM ou chip settings. Termina com RESET no conversor.

import importlib.util
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "oracle", os.path.join(HERE, "afe-spi-oracle.py"))
oracle = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(oracle)

oracle.ALLOWED_BRIDGE |= {0x20, 0x31}   # só leituras

DRDY_PIN = 5
CS_PIN = oracle.CS_PIN
PASS = FAIL = 0


def ok(msg):
    global PASS
    PASS += 1
    print(f"  PASS  {msg}")


def no(msg):
    global FAIL
    FAIL += 1
    print(f"  FAIL  {msg}")


def pins(b):
    rx = b.cmd(bytes([0x31]))
    return int.from_bytes(rx[4:6], "little")


def drdy_samples(b, n):
    """n leituras do GP5; devolve (altos, baixos)."""
    hi = sum((pins(b) >> DRDY_PIN) & 1 for _ in range(n))
    return hi, n - hi


def miso_probe(b, active_mask, nbytes=4):
    """Lê nbytes com a máscara de CS dada; 0x01FF = nenhum CS ativo."""
    t = bytearray(21)
    t[0] = 0x40
    t[4:8] = b.rate.to_bytes(4, "little")
    t[8:10] = (0x01FF).to_bytes(2, "little")
    t[10:12] = active_mask.to_bytes(2, "little")
    t[18:20] = nbytes.to_bytes(2, "little")
    t[20] = oracle.SPI_MODE
    rx = b.cmd(t)
    if rx[1] != 0:
        oracle.die(f"0x40 recusado, estado 0x{rx[1]:02x}")
    return b.xfer(bytes(nbytes), settings=False)


def read_id(b, mode, rate):
    oracle.SPI_MODE, b.rate = mode, rate
    try:
        b.xfer(bytes([oracle.SDATAC]))
        return oracle.rreg_all(b)[0]
    finally:
        oracle.SPI_MODE, b.rate = 1, oracle.BIT_RATE


def main():
    b = oracle.Bridge()
    print(f"# afe-alive em {b.dev}: CS=GP{CS_PIN}, DRDY=GP{DRDY_PIN}")

    print("\n== 1. A ponte enxerga o GP5 como entrada GPIO?")
    rx = b.cmd(bytes([0x20]))
    desig = rx[4:13]
    dirs = int.from_bytes(rx[15:17], "little")
    print("  designação GP0..GP8: " + " ".join(str(d) for d in desig)
          + "   (0 GPIO, 1 CS, 2 dedicado)")
    if desig[DRDY_PIN] == 0 and (dirs >> DRDY_PIN) & 1:
        ok("GP5 é GPIO de entrada: o nível lido é o que o DRDY dirige")
    else:
        no(f"GP5 designação {desig[DRDY_PIN]}, direção {(dirs >> DRDY_PIN) & 1}: "
           "o teste do DRDY não vale nesta configuração")
    if desig[CS_PIN] == 1:
        ok("GP4 é chip select")
    else:
        no(f"GP4 designação {desig[CS_PIN]}: não é chip select")
    print(f"  níveis agora: 0x{pins(b):03x}  (GP{DRDY_PIN} = {(pins(b) >> DRDY_PIN) & 1})")

    print("\n== 2. A MISO com e sem o CS ativo")
    idle = miso_probe(b, 0x01FF)
    act = miso_probe(b, 0x01FF & ~(1 << CS_PIN))
    print(f"  CS inativo: {idle.hex(' ')}    CS ativo (GP4): {act.hex(' ')}")
    if idle == act:
        print("  iguais: com o CS ativo, nada muda na MISO")
    else:
        ok("a MISO muda quando o GP4 ativa: alguém do outro lado responde")

    print("\n== 3. O ID em cada modo SPI, a 100 kHz e a 1 MHz")
    seen = set()
    for mode in range(4):
        for rate in (100_000, 1_000_000):
            v = read_id(b, mode, rate)
            seen.add(v)
            print(f"  modo {mode}, {rate // 1000:4d} kHz: ID 0x{v:02x}")
    if seen - {0xFF, 0x00}:
        ok(f"algum modo leu um valor que não é 0x00/0xff: {sorted(hex(v) for v in seen)}")
    else:
        no("todos os modos e velocidades leem só 0xff/0x00: não é modo nem velocidade")

    print("\n== 4. O DRDY responde a START/STOP? (independe da MISO)")
    b.xfer(bytes([oracle.RESET]))
    time.sleep(0.01)                     # 18 tCLK bastam; 10 ms de folga
    b.xfer(bytes([oracle.SDATAC]))
    b.xfer(bytes([oracle.STOP]))
    time.sleep(0.01)
    hi0, lo0 = drdy_samples(b, 50)
    print(f"  parado:          {hi0:3d} alto / {lo0:3d} baixo   (esperado: alto)")
    b.xfer(bytes([oracle.START]))
    time.sleep(0.05)                     # 250 SPS: primeira amostra em ~16 ms
    hi1, lo1 = drdy_samples(b, 200)
    print(f"  depois do START: {hi1:3d} alto / {lo1:3d} baixo   (esperado: baixo)")
    b.xfer(bytes([oracle.STOP]))
    b.xfer(bytes([oracle.RESET]))
    time.sleep(0.01)
    hi2, lo2 = drdy_samples(b, 50)
    print(f"  depois do RESET: {hi2:3d} alto / {lo2:3d} baixo   (esperado: alto)")

    alive = hi0 > 45 and lo1 > 180
    if alive:
        ok("o DRDY desce com o START: o conversor está alimentado, com clock, "
           "e recebe comandos")
    else:
        no("o DRDY não acompanhou o START")

    b.close()

    print("\n== Leitura")
    if alive:
        print("  O chip está VIVO. Ele converte, mas o que ele põe no DOUT não chega à")
        print("  MISO da ponte: continuidade DOUT->MISO, solda do pino DOUT, ou o")
        print("  próprio buffer de saída do DOUT danificado (o único 'queimado' compatível).")
    elif lo0 == 0 and lo1 == 0:
        print("  O DRDY ficou ALTO o tempo todo: o chip nunca converteu. Compatível com")
        print("  chip sem alimentação, sem clock, com RESET/PWDN baixos, com CS/SCLK/MOSI")
        print("  sem chegar - ou queimado. O software não separa isso: meça AVDD, DVDD,")
        print("  PWDN, RESET e o clock. Só com tudo isso correto o chip fica como suspeito.")
    elif hi0 == 0 and hi1 == 0:
        print("  O DRDY ficou BAIXO o tempo todo: linha em curto com o terra, ou o pino")
        print("  DRDY do chip preso. Meça o GP5 com a placa desligada (resistência ao GND).")
    else:
        print("  O DRDY mudou, mas não como esperado. Cole a saída inteira.")
    print(f"\n  {PASS} PASS, {FAIL} FAIL. Conversor resetado; da ponte só a RAM de SPI foi tocada.")


if __name__ == "__main__":
    main()
