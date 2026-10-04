#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Oraculo de bancada do conversor: fala com ele pelo MCP2210 por /dev/hidraw,
# SEM o hid-mcp2210 e SEM o ti-ads1299 no caminho.
#
# Existe para separar "o silicio faz X" de "o nosso driver faz X". Os drivers
# leem e escrevem os registradores por um regmap com cache, e um registrador
# escrito nunca e relido do hardware; este programa rele tudo do silicio. E
# uma implementacao independente do protocolo dos dois chips - escrita a partir
# das transcricoes, nao do codigo dos drivers -, entao concordar com ele e
# evidencia e discordar dele aponta o lado.
#
# Roda no HOST, com a ponte no hid-generic (sudo rmmod hid_mcp2210).
#
#   sudo python3 scripts/afe-spi-oracle.py            # regs, escrita, sinal de teste
#   sudo python3 scripts/afe-spi-oracle.py regs       # so o mapa de registradores
#   sudo python3 scripts/afe-spi-oracle.py diff       # modo x velocidade, como o driver
#   sudo python3 scripts/afe-spi-oracle.py single     # por que single-shot le ruido
#   sudo python3 scripts/afe-spi-oracle.py timing     # onde estao os ms de cada amostra
#
# O que ele envia, e nada mais:
#   ponte:     0x40 (transfer settings, RAM - volatil) e 0x42 (transferencia)
#   conversor: SDATAC, RREG, WREG em CONFIG2/CONFIG3/CHnSET, START, STOP,
#              RDATA, RESET. Termina SEMPRE com RESET: o conversor volta ao
#              estado de power-up, como se nada tivesse acontecido.
# Nunca: NVRAM da ponte (0x60), chip settings (0x21), senha (0x70).
#
# Placa: CS no GP4 (provisionado, BRINGUP_AFE.md §2.3). SPI modo 1, 1 MHz:
# um byte leva 8 us, acima dos 4 tCLK (~2 us) que o conversor pede entre os
# bytes de um comando de varios bytes.

import importlib.util
import os
import select
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "snap", os.path.join(HERE, "mcp2210-nvram-snapshot.py"))
snap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(snap)

REPORT = 64
CS_PIN = 4
BIT_RATE = 1_000_000            # registradores: abaixo de f_CLK, com folga
SPI_MODE = 1
ALLOWED_BRIDGE = {0x40, 0x41, 0x42}   # 0x41 so le as transfer settings

# Conversor (SBAS499, transcrito em docs/Register_Map_ADS1299.md)
SDATAC, RDATA, START, STOP, RESET = 0x11, 0x12, 0x08, 0x0A, 0x06
RREG, WREG = 0x20, 0x40
NREGS = 0x18
NAMES = ["ID", "CONFIG1", "CONFIG2", "CONFIG3", "LOFF"] + \
        [f"CH{i}SET" for i in range(1, 9)] + \
        ["BIAS_SENSP", "BIAS_SENSN", "LOFF_SENSP", "LOFF_SENSN", "LOFF_FLIP",
         "LOFF_STATP", "LOFF_STATN", "GPIO", "MISC1", "MISC2", "CONFIG4"]
# Valores de reset da Tabela 11. O ID nao tem: depende da revisao.
RESET_VALUES = {1: 0x96, 2: 0xC0, 3: 0x60, 4: 0x00, **{r: 0x61 for r in range(5, 13)},
                **{r: 0x00 for r in range(13, 20)}, 0x14: 0x0F, 0x15: 0x00,
                0x16: 0x00, 0x17: 0x00}
# Bits que o silicio escreve sozinho: nao sao configuracao e nao entram em
# nenhuma comparacao. CONFIG3 bit 0 e BIAS_STAT; LOFF_STATP/N sao o estado dos
# comparadores de lead-off. A primeira versao os comparava e acusou FAIL e
# "leitura diferente" numa peca que estava certa (BRINGUP_AFE.md §4).
STATUS_MASK = {0x03: 0x01, 0x12: 0xFF, 0x13: 0xFF}


def cfg(reg, v):
    return v & ~STATUS_MASK.get(reg, 0) & 0xFF


def die(msg):
    print(f"ERRO: {msg}", file=sys.stderr)
    sys.exit(2)


class Bridge:
    def __init__(self):
        dev, usb = snap.find_bridge()          # recusa se nao for hid-generic
        self.fd = os.open(dev, os.O_RDWR)
        while select.select([self.fd], [], [], 0)[0]:
            os.read(self.fd, REPORT)
        self.dev = dev
        self.rate = BIT_RATE
        self.exchanges = 0
        self.busy = 0

    def close(self):
        os.close(self.fd)

    def cmd(self, tx):
        if tx[0] not in ALLOWED_BRIDGE:
            raise AssertionError(f"recusado: comando 0x{tx[0]:02x} da ponte")
        buf = bytearray(REPORT)
        buf[:len(tx)] = tx
        self.exchanges += 1
        os.write(self.fd, b"\x00" + bytes(buf))
        if not select.select([self.fd], [], [], 1.0)[0]:
            die(f"sem resposta ao 0x{tx[0]:02x}")
        rx = os.read(self.fd, REPORT)
        if rx[0] != tx[0]:
            die(f"eco errado: pedi 0x{tx[0]:02x}, veio 0x{rx[0]:02x}")
        return rx

    def settings(self, nbytes):
        """Transfer settings em RAM (Tabela 3-35) para uma transacao de n bytes."""
        t = bytearray(21)
        t[0] = 0x40
        t[4:8] = self.rate.to_bytes(4, "little")
        t[8:10] = (0x01FF).to_bytes(2, "little")                 # ocioso: todos altos
        t[10:12] = (0x01FF & ~(1 << CS_PIN)).to_bytes(2, "little")  # ativo: GP4 baixo
        # 12..17: atrasos CS->dado, dado->CS, entre bytes = 0
        t[18:20] = nbytes.to_bytes(2, "little")
        t[20] = SPI_MODE
        rx = self.cmd(t)
        if rx[1] != 0x00:
            die(f"0x40 recusado, estado 0x{rx[1]:02x}")

    def xfer(self, data, settings=True):
        """Uma transacao SPI completa, com CS ativo do primeiro ao ultimo byte.

        settings=False pula o 0x40: so vale se as transfer settings em RAM ja
        estao com este tamanho - e o que a medida de temporizacao compara.
        """
        if not 0 < len(data) <= 60:
            raise ValueError("este oraculo so faz transacoes de 1 a 60 bytes")
        if settings:
            self.settings(len(data))
        out, sent = bytearray(), False
        for _ in range(200):
            payload = b"" if sent else bytes(data)
            rx = self.cmd(bytes([0x42, len(payload), 0, 0]) + payload)
            if rx[1] == 0xF8:            # transferencia em curso, tente de novo
                self.busy += 1
                time.sleep(0.0001)       # o driver dorme 100-200 us
                continue
            if rx[1] != 0x00:
                die(f"0x42 estado 0x{rx[1]:02x}")
            sent = True
            out += rx[4:4 + rx[2]]
            if rx[3] == 0x10:            # terminada (Tabela 3-63)
                break
        else:
            die("a transferencia nao terminou")
        if len(out) != len(data):
            die(f"enviei {len(data)} bytes e recebi {len(out)}")
        return bytes(out)


def rreg_all(b):
    rx = b.xfer(bytes([RREG | 0, NREGS - 1]) + bytes(NREGS))
    return rx[2:]


def wreg(b, reg, val):
    b.xfer(bytes([WREG | reg, 0, val]))


def show_regs(regs, title, against_reset):
    print(f"\n== {title}")
    bad = 0
    for r, v in enumerate(regs):
        exp = RESET_VALUES.get(r)
        note = ""
        if r in STATUS_MASK:
            note = "(status, nao comparado)" if STATUS_MASK[r] == 0xFF else "(bit 0 e status)"
        if against_reset and exp is not None and STATUS_MASK.get(r) != 0xFF:
            if cfg(r, v) == cfg(r, exp):
                note = ("reset " + note).strip()
            else:
                note = f"<- reset e 0x{exp:02x} {note}".strip()
                bad += 1
        print(f"  0x{r:02x} {NAMES[r]:11s} 0x{v:02x}  {note}")
    return bad


def decode_id(v):
    family_ok = (v >> 2) & 0x7 == 0x7
    nch = 4 + 2 * (v & 0x3)
    print(f"\n  ID 0x{v:02x}: REV_ID={v >> 5}, bit4={(v >> 4) & 1}, DEV_ID={(v >> 2) & 3}, "
          f"NU_CH={v & 3} -> {nch} canais; familia {'confere' if family_ok else 'NAO confere'}")


def write_test(b):
    print("\n== Escrita e releitura (o que o regmap do driver nunca faz)")
    ok = True
    for reg, val in ((0x02, 0xD0), (0x05, 0x65), (0x03, 0xE0)):
        before = rreg_all(b)[reg]
        wreg(b, reg, val)
        after = rreg_all(b)[reg]
        same = cfg(reg, after) == cfg(reg, val)
        ok &= same
        print(f"  {'PASS' if same else 'FAIL'}  {NAMES[reg]}: era 0x{before:02x}, "
              f"escrevi 0x{val:02x}, li 0x{after:02x}")
    return ok


def to_signed24(b3):
    v = int.from_bytes(b3, "big")
    return v - (1 << 24) if v & 0x800000 else v


def signal_test(b, readings=40, single_shot=False, rdata_rate=BIT_RATE, quiet=False):
    say = (lambda *a, **k: None) if quiet else print
    say("\n== Sinal de teste interno, lido pelo oraculo")
    wreg(b, 0x03, 0xE0)          # PD_REFBUF=1: referencia interna ligada
    time.sleep(0.2)              # o buffer de referencia precisa assentar
    wreg(b, 0x02, 0xD0)          # INT_CAL=1, 1x, f_CLK/2^21 (~1 Hz)
    for ch in range(8):
        wreg(b, 0x05 + ch, 0x65)  # ganho 24, MUX=101 sinal de teste
    wreg(b, 0x17, 0x08 if single_shot else 0x00)   # CONFIG4: SINGLE_SHOT
    regs = rreg_all(b)
    say(f"  conferido antes de medir: CONFIG2=0x{regs[2]:02x} CONFIG3=0x{regs[3]:02x} "
        f"CH1SET=0x{regs[5]:02x} .. CH8SET=0x{regs[12]:02x} CONFIG4=0x{regs[0x17]:02x}")
    if not single_shot:
        b.xfer(bytes([START]))
        time.sleep(0.05)
    lo, hi, status = [None] * 8, [None] * 8, set()
    for i in range(readings + 2):
        if single_shot:                 # o que o driver faz: START, 20 ms, RDATA
            b.xfer(bytes([START]))
            time.sleep(0.02)
        b.rate = rdata_rate
        try:
            rx = b.xfer(bytes([RDATA]) + bytes(27))
        finally:
            b.rate = BIT_RATE
        time.sleep(0.05)
        if i < 2:
            continue
        status.add(rx[1:4].hex())
        for ch in range(8):
            v = to_signed24(rx[4 + 3 * ch: 7 + 3 * ch])
            lo[ch] = v if lo[ch] is None else min(lo[ch], v)
            hi[ch] = v if hi[ch] is None else max(hi[ch], v)
    b.xfer(bytes([STOP]))
    pp = [hi[c] - lo[c] for c in range(8)]
    say(f"  palavra de status vista: {sorted(status)} (os 4 bits altos devem ser 1100)")
    for ch in range(8):
        say(f"  CH{ch + 1}: {lo[ch]:>9d} .. {hi[ch]:>9d}  pico a pico {pp[ch]:>8d} codigos")
    if quiet:
        return pp, sorted(status)
    # A expectativa do driver, sem os numeros do driver: ganho * 2^23 / 1200 e
    # a leitura "VREF/1200 pico a pico"; a outra leitura da Tabela 14 da metade.
    full, half = 24 * (1 << 23) // 1200, 24 * (1 << 23) // 2400
    say(f"  referencia: {full} codigos se o sinal for VREF/1200 pp, {half} se for VREF/2400 pp "
        f"(a ambiguidade A1)")
    return pp


def levels(b, mux, config2, single_shot, readings=20):
    """Media e extremos do CH1 com um MUX e um CONFIG2 dados, num modo dado."""
    wreg(b, 0x03, 0xE0)
    time.sleep(0.2)
    wreg(b, 0x02, config2)
    for ch in range(8):
        wreg(b, 0x05 + ch, 0x60 | mux)
    wreg(b, 0x17, 0x08 if single_shot else 0x00)
    if not single_shot:
        b.xfer(bytes([START]))
        time.sleep(0.05)
    vals = []
    for i in range(readings + 2):
        if single_shot:
            b.xfer(bytes([START]))
            time.sleep(0.02)
        rx = b.xfer(bytes([RDATA]) + bytes(27))
        time.sleep(0.05)
        if i >= 2:
            vals.append(to_signed24(rx[4:7]))
    b.xfer(bytes([STOP]))
    return min(vals), sum(vals) // len(vals), max(vals)


def single_test(b):
    """Por que single-shot le ruido: o gerador perde a fase, ou a conversao esta errada?"""
    print("\n== Single-shot: o que muda, e o que nao muda (CH1, ganho 24 exceto MVDD)")
    print("   (a) o START reinicia o gerador: single-shot fica preso num nivel, +-83000")
    print("   (b) a conversao single-shot esta errada: niveis perto de 0, ou DC diferente")
    cases = (
        ("sinal de teste ~1 Hz", 0x5, 0xD0),
        ("sinal de teste DC   ", 0x5, 0xD3),   # CAL_FREQ = 11: nivel constante
        ("entradas em curto   ", 0x1, 0xC0),
    )
    for name, mux, c2 in cases:
        for single in (False, True):
            lo, mean, hi = levels(b, mux, c2, single)
            mode = "single-shot" if single else "continuo   "
            print(f"  {name} {mode}: min {lo:>8d}  media {mean:>8d}  max {hi:>8d}")
    # MVDD com ganho 1: uma tensao de alimentacao, independente do gerador
    for single in (False, True):
        wreg(b, 0x05, 0x03)       # CH1: ganho 1, MUX=011 MVDD
        lo, mean, hi = levels_g1(b, single)
        mode = "single-shot" if single else "continuo   "
        print(f"  MVDD, ganho 1        {mode}: min {lo:>8d}  media {mean:>8d}  max {hi:>8d}")


def levels_g1(b, single_shot, readings=20):
    wreg(b, 0x03, 0xE0)
    time.sleep(0.2)
    wreg(b, 0x02, 0xC0)
    wreg(b, 0x05, 0x03)
    wreg(b, 0x17, 0x08 if single_shot else 0x00)
    if not single_shot:
        b.xfer(bytes([START]))
        time.sleep(0.05)
    vals = []
    for i in range(readings + 2):
        if single_shot:
            b.xfer(bytes([START]))
            time.sleep(0.02)
        rx = b.xfer(bytes([RDATA]) + bytes(27))
        time.sleep(0.05)
        if i >= 2:
            vals.append(to_signed24(rx[4:7]))
    b.xfer(bytes([STOP]))
    return min(vals), sum(vals) // len(vals), max(vals)


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


def timing_test(b, n=300):
    """Onde estao os ~6 ms por amostra do link usb (BRINGUP_AFE.md §6)."""
    print(f"\n== Temporizacao pela ponte, {n} repeticoes de cada, sem driver")
    rdata = bytes([RDATA]) + bytes(27)        # o quadro do driver: 28 bytes
    b.rate = 4_000_000                        # a velocidade que o driver usa no RDATA

    def run(label, fn):
        times, ex, busy = [], [], []
        for _ in range(n):
            e0, b0 = b.exchanges, b.busy
            t0 = time.perf_counter_ns()
            fn()
            times.append((time.perf_counter_ns() - t0) / 1e6)
            ex.append(b.exchanges - e0)
            busy.append(b.busy - b0)
        med = pct(times, 0.5)
        print(f"  {label}: mediana {med:.3f} ms, p90 {pct(times, 0.9):.3f}, max {max(times):.3f}; "
              f"trocas HID {min(ex)}..{max(ex)} (media {sum(ex) / n:.2f}), "
              f"repeticoes 0xF8 {sum(busy)}; teto {1000 / med:.0f}/s")
        return med

    one = run("(a) uma troca HID (0x41)        ", lambda: b.cmd(bytes([0x41])))
    full = run("(b) RDATA como o driver: 0x40+0x42", lambda: b.xfer(rdata, settings=True))
    b.settings(len(rdata))
    lean = run("(c) RDATA sem reenviar o 0x40    ", lambda: b.xfer(rdata, settings=False))
    b.rate = BIT_RATE
    print(f"  250 SPS da 4.000 ms por amostra. (b) usa {full / 4:.0%} disso; (c) usa {lean / 4:.0%}.")
    print(f"  custo do 0x40 por amostra: {full - lean:.3f} ms; de uma troca: {one:.3f} ms")


def diff_test(b):
    """As duas diferencas entre o driver e o oraculo, uma de cada vez."""
    print("\n== Driver contra oraculo: modo de conversao x velocidade do RDATA")
    print("   (o driver usa single-shot com START por leitura e RDATA a ~4 MHz)")
    for single, rate in ((False, 1_000_000), (True, 1_000_000),
                         (False, 4_000_000), (True, 4_000_000)):
        pp, status = signal_test(b, readings=30, single_shot=single,
                                 rdata_rate=rate, quiet=True)
        mode = "single-shot" if single else "continuo   "
        print(f"  {mode} RDATA a {rate / 1e6:.0f} MHz: pico a pico {min(pp):>7d} .. {max(pp):>7d} "
              f"codigos, status {status}")


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    b = Bridge()
    print(f"# oraculo em {b.dev}: CS=GP{CS_PIN}, modo {SPI_MODE}, {BIT_RATE} Hz")
    try:
        b.xfer(bytes([SDATAC]))          # sai do RDATAC, se estiver nele
        time.sleep(0.01)
        # Primeiro como encontrado: e o rastro de quem mexeu por ultimo (o
        # driver, se ele carregou), e mostra se as escritas dele chegaram.
        show_regs(rreg_all(b), "Como encontrado (o que o ultimo a mexer deixou)", False)
        # Depois do RESET: o criterio 2 da Fase 3, contra a Tabela 11.
        b.xfer(bytes([RESET]))
        time.sleep(0.01)
        b.xfer(bytes([SDATAC]))
        time.sleep(0.01)
        regs = rreg_all(b)
        bad = show_regs(regs, "Depois de RESET, contra a Tabela 11", True)
        print(f"  {'PASS' if bad == 0 else 'FAIL'}  {bad} registrador(es) de configuracao fora do reset")
        decode_id(regs[0])
        again = rreg_all(b)
        stable = all(cfg(r, again[r]) == cfg(r, regs[r]) for r in range(NREGS)
                     if STATUS_MASK.get(r) != 0xFF)
        print(f"  {'PASS' if stable else 'FAIL'}  segunda leitura "
              f"{'identica' if stable else 'DIFERENTE: ' + again.hex()} (status excluido)")
        if what in ("all", "write"):
            write_test(b)
        if what in ("all", "signal"):
            signal_test(b)
        if what in ("all", "diff"):
            diff_test(b)
        if what == "single":
            single_test(b)
        if what == "timing":
            timing_test(b)
    finally:
        b.xfer(bytes([RESET]))           # de volta ao estado de power-up
        time.sleep(0.01)
        b.close()
        print("\n  conversor resetado (comando RESET); ponte so teve a RAM tocada")


if __name__ == "__main__":
    main()
