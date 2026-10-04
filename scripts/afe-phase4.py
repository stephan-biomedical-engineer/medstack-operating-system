#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Fase 4 do implementation_plan_afe_bench.md: o caminho analogico, estatico.
#
# So sysfs e /dev/iio:deviceN - nada de hidraw, nada de driver por fora. Roda
# no host ou na placa, como root, com o front-end ja registrado (Fase 3).
#
#   sudo python3 scripts/afe-phase4.py
#   sudo python3 scripts/afe-phase4.py --avdd 5.00     # AVDD medido no multimetro
#
# A Fase 4 pede fonte DC de precisao, atenuador e termometro. Sem eles, este
# roteiro usa o que o conversor tem dentro, e diz em cada secao o que isso
# troca:
#
#   8.1 ganho     o gerador interno no lugar da fonte DC externa. Ele vale
#                 +-VREF/2400 (medido em BRINGUP_AFE.md §4); lido por
#                 in_voltage0_raw, que e single-shot, ele fica sempre na fase 0,
#                 ou seja, num nivel DC - e e isso que se usa aqui, descontado o
#                 offset de entradas em curto no mesmo ganho.
#   8.3 gerador   pelo BUFFER, que converte em modo continuo. Pelo raw, a onda
#                 de ~1 Hz nunca se move (o mesmo mecanismo que derrubou o
#                 autoteste de probe); o procedimento do plano, que le o raw,
#                 mediria uma linha reta.
#   8.4 ruido     leituras avulsas, ~10 por segundo pela USB, e nao 10 s a 250
#                 SPS. O pico a pico de N conversoes vale; o espectro, nao.
#   8.5 MVDD      contra --avdd, se dado. Temperatura: so a tensao, porque a
#                 formula do datasheet nao esta transcrita.
#   injecao       shorted com o gerador ligado tem de ler o offset, nao o
#                 gerador - a injecao que o plano chama de a mais importante.
#
# Deixa o dispositivo como encontrou: buffer desligado, gerador desligado,
# MUX normal, ganho e taxa originais.

import argparse
import glob
import os
import select
import struct
import time

GAINS = (1, 2, 4, 6, 8, 12, 24)
VREF_MV = 4500.0
GEN_PEAK_MV = VREF_MV / 2400          # medido: +-VREF/2400 (BRINGUP_AFE.md §4.3)


def find_dev():
    for d in sorted(glob.glob("/sys/bus/iio/devices/iio:device*")):
        try:
            if open(f"{d}/name").read().strip().startswith("ads1299"):
                return d
        except OSError:
            pass
    raise SystemExit("ERRO: nenhum iio:device com nome ads1299-*. A Fase 3 passou?")


class Afe:
    def __init__(self, d):
        self.d = d
        self.node = "/dev/" + os.path.basename(d)

    def rd(self, a):
        with open(f"{self.d}/{a}") as f:
            return f.read().strip()

    def wr(self, a, v):
        with open(f"{self.d}/{a}", "w") as f:
            f.write(str(v))

    def raw(self, n, ch=0):
        return [int(self.rd(f"in_voltage{ch}_raw")) for _ in range(n)]

    def scale_mv(self, ch=0):
        return float(self.rd(f"in_voltage{ch}_scale"))


def stats(v):
    m = sum(v) / len(v)
    rms = (sum((x - m) ** 2 for x in v) / len(v)) ** 0.5
    return m, max(v) - min(v), rms


def verdict(ok):
    return "PASS" if ok else "FAIL"


def sec_gain(a, n):
    print("\n== 8.1 Varredura de ganho, com o gerador interno como fonte DC")
    print("   amp = (nivel com test_signal - nivel em curto) x scale, nos dois amplitudes")
    print(f"   esperado: +{GEN_PEAK_MV:.4f} mV (1x) e +{2 * GEN_PEAK_MV:.4f} mV (2x), constante nos 7 ganhos")
    results = {}
    for amp_name, mult in (("1x_slow", 1), ("2x_slow", 2)):
        vals = []
        for g in GAINS:
            a.wr("hardwaregain", g)
            a.wr("test_signal", "off")
            a.wr("input_mux", "shorted")
            off, _, _ = stats(a.raw(n))
            a.wr("test_signal", amp_name)
            a.wr("input_mux", "test_signal")
            lvl, lvl_pp, _ = stats(a.raw(n))
            sc = a.scale_mv()
            mv = (lvl - off) * sc
            vals.append(mv)
            print(f"  {amp_name} ganho {g:>2}: offset {off:>10.0f}  nivel {lvl:>10.0f} "
                  f"(pp {lvl_pp:>4d})  scale {sc:.9f}  -> {mv:+.4f} mV")
        spread = (max(vals) - min(vals)) / (sum(vals) / len(vals)) * 100
        err = (sum(vals) / len(vals) - mult * GEN_PEAK_MV) / (mult * GEN_PEAK_MV) * 100
        print(f"  {verdict(spread <= 1.0)}  {amp_name}: dispersao entre ganhos {spread:.2f}% (criterio 1%)")
        print(f"  {verdict(abs(err) <= 5.0)}  {amp_name}: media {err:+.2f}% do esperado (criterio 5%)")
        results[amp_name] = vals
    r = [b / a_ for a_, b in zip(results["1x_slow"], results["2x_slow"])]
    print(f"  info  razao 2x/1x por ganho: {' '.join(f'{x:.4f}' for x in r)} (esperado 2)")
    a.wr("test_signal", "off")


def sec_noise(a, n):
    print(f"\n== 8.4 Entradas em curto: piso de ruido, {n} conversoes avulsas por ganho")
    for g in GAINS:
        a.wr("hardwaregain", g)
        a.wr("input_mux", "shorted")
        v = a.raw(n)
        m, pp, rms = stats(v)
        sc_uv = a.scale_mv() * 1000
        print(f"  ganho {g:>2}: offset {m * sc_uv:>9.2f} uV   pp {pp * sc_uv:>7.3f} uV   "
              f"rms {rms * sc_uv:>6.3f} uV   ({pp} codigos pp)")
    print("  (o criterio do plano compara com a tabela de ruido do datasheet, nao transcrita;")
    print("   o limite de 'nao esta quebrado' do framework e 10 uV pp)")


def sec_supply_temp(a, n, avdd):
    print("\n== 8.5 MVDD e temperatura, ganho 1")
    a.wr("hardwaregain", 1)
    a.wr("input_mux", "supply")
    m, pp, _ = stats(a.raw(n))
    v = m * a.scale_mv() / 1000
    print(f"  MVDD CH1: {v:.4f} V (pp {pp} codigos)")
    if avdd:
        exp = avdd / 2
        err = (v - exp) / exp * 100
        print(f"  {verdict(abs(err) <= 2.0)}  contra AVDD/2 = {exp:.4f} V do multimetro: {err:+.2f}% "
              "(criterio 2%; supoe que o CH1 mede (AVDD+AVSS)/2, nao transcrito)")
    else:
        print("  info  sem --avdd: se o CH1 mede (AVDD+AVSS)/2, AVDD = "
              f"{2 * v:.3f} V (a conferir no multimetro)")
    a.wr("input_mux", "temperature")
    m, pp, _ = stats(a.raw(n))
    uv = m * a.scale_mv() * 1000
    print(f"  temperatura: {uv:.0f} uV (pp {pp} codigos). A formula do datasheet nao esta")
    print("  transcrita; pela que se le em SBAS499 [SBAS499? conferir]: "
          f"{(uv - 145300) / 490 + 25:.1f} C")


def sec_injection(a, n):
    print("\n== Injecao de falha: MUX em curto com o gerador LIGADO")
    a.wr("hardwaregain", 24)
    a.wr("test_signal", "off")
    a.wr("input_mux", "shorted")
    off, _, rms = stats(a.raw(n))
    a.wr("test_signal", "2x_slow")
    inj, _, _ = stats(a.raw(n))
    a.wr("input_mux", "test_signal")
    seen, _, _ = stats(a.raw(n))
    a.wr("test_signal", "off")
    a.wr("input_mux", "shorted")
    moved = abs(inj - off)
    print(f"  curto, gerador desligado: {off:.0f}   curto, gerador 2x: {inj:.0f}   "
          f"test_signal, gerador 2x: {seen:.0f}")
    print(f"  {verdict(moved <= max(5 * rms, 50))}  em curto a leitura nao seguiu o gerador "
          f"(moveu {moved:.0f} codigos; o gerador vale {seen - off:.0f})")


def s24(v):
    v &= 0xFFFFFF
    return v - (1 << 24) if v & 0x800000 else v


def read_buffer(a, seconds):
    """Os 8 canais + timestamp, pelo buffer (modo continuo). Devolve [(t_ns, raw do CH1)].

    Todos habilitados de proposito: o driver empurra a varredura inteira, e
    com todos os canais o layout e o da struct dele - 8 x le:s24/32 e o
    timestamp s64 no byte 32 - sem depender de demux do nucleo IIO.
    """
    b = f"{a.d}/buffer0"
    for f in glob.glob(f"{b}/in_*_en"):
        open(f, "w").write("1")
    open(f"{b}/length", "w").write("4096")
    fmt, size = "<8Iq", 40
    fd = os.open(a.node, os.O_RDONLY | os.O_NONBLOCK)
    out, buf = [], b""
    try:
        open(f"{b}/enable", "w").write("1")
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if select.select([fd], [], [], 0.2)[0]:
                try:
                    buf += os.read(fd, 4096)
                except BlockingIOError:
                    pass
            while len(buf) >= size:
                fields = struct.unpack(fmt, buf[:size])
                buf = buf[size:]
                out.append((fields[8], s24(fields[0])))
    finally:
        open(f"{b}/enable", "w").write("0")
        os.close(fd)
    return out


def sec_generator_ac(a, seconds):
    print(f"\n== 8.3 O gerador, pelo buffer (modo continuo), {seconds:.0f} s cada, ganho 24")
    a.wr("hardwaregain", 24)
    sc = a.scale_mv() * 1000
    expect = {"1x_slow": (2 * GEN_PEAK_MV * 1000, 2048000 / 2 ** 21),
              "1x_fast": (2 * GEN_PEAK_MV * 1000, 2048000 / 2 ** 20),
              "2x_slow": (4 * GEN_PEAK_MV * 1000, 2048000 / 2 ** 21),
              "2x_fast": (4 * GEN_PEAK_MV * 1000, 2048000 / 2 ** 20)}
    a.wr("input_mux", "test_signal")
    for name, (pp_uv, f_hz) in expect.items():
        a.wr("test_signal", name)
        s = read_buffer(a, seconds)
        # lost_samples e zerado a cada vez que o buffer liga (postenable), entao
        # o valor lido depois e o desta janela - nao uma diferenca. A primeira
        # versao subtraia o valor anterior e relatou +0 nas janelas 2 a 4.
        lost = int(a.rd("lost_samples"))
        if len(s) < 20:
            print(f"  {name}: so {len(s)} amostras - nada a medir")
            continue
        vals = sorted(v for _, v in s)
        lo, hi = vals[len(vals) // 50], vals[-len(vals) // 50 - 1]   # 2% e 98%
        mid = (lo + hi) / 2
        # frequencia: bordas de subida pelo meio, com os timestamps do kernel
        edges = [s[i][0] for i in range(1, len(s)) if s[i - 1][1] < mid <= s[i][1]]
        span_s = (s[-1][0] - s[0][0]) / 1e9
        rate = len(s) / span_s if span_s > 0 else 0
        freq = ((len(edges) - 1) / ((edges[-1] - edges[0]) / 1e9)) if len(edges) >= 2 else 0
        pp = (hi - lo) * sc
        e_pp = (pp - pp_uv) / pp_uv * 100
        due = round(span_s * int(a.rd("sampling_frequency")))
        print(f"  {name}: {len(s)} amostras em {span_s:.2f} s ({rate:.0f}/s), lost_samples {lost}; "
              f"entregues + perdidas = {len(s) + lost}, devidas ~{due}")
        print(f"     {verdict(abs(e_pp) <= 5)}  pico a pico {pp:.1f} uV, esperado {pp_uv:.0f} ({e_pp:+.2f}%)")
        if freq:
            e_f = (freq - f_hz) / f_hz * 100
            print(f"     info  frequencia {freq:.4f} Hz, esperado {f_hz:.4f} Hz ({e_f:+.2f}%; "
                  "tolerancia do oscilador interno, a declarar)")
        else:
            print("     info  menos de duas bordas: janela curta demais para a frequencia")
    a.wr("test_signal", "off")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--avdd", type=float, help="AVDD medido no multimetro, em volts")
    ap.add_argument("-n", type=int, default=20, help="leituras avulsas por ponto")
    ap.add_argument("--seconds", type=float, default=5.0, help="janela do buffer por ajuste")
    ap.add_argument("--only", choices=("gain", "noise", "supply", "injection", "ac"),
                    help="roda so uma secao (ac = o gerador pelo buffer, e a taxa entregue)")
    args = ap.parse_args()

    a = Afe(find_dev())
    saved = {k: a.rd(k) for k in ("hardwaregain", "input_mux", "test_signal", "sampling_frequency")}
    print(f"# Fase 4 em {a.d} ({a.rd('name')}), {a.rd('sampling_frequency')} SPS, "
          f"timestamp {a.rd('timestamp_source')}")
    print(f"# estado encontrado: {saved}")
    try:
        run = lambda k: args.only in (None, k)
        if run("gain"):
            sec_gain(a, args.n)
        if run("noise"):
            sec_noise(a, args.n * 2)
        if run("supply"):
            sec_supply_temp(a, args.n, args.avdd)
        if run("injection"):
            sec_injection(a, args.n)
        if run("ac"):
            sec_generator_ac(a, args.seconds)
    finally:
        try:
            open(f"{a.d}/buffer0/enable", "w").write("0")
        except OSError:
            pass
        a.wr("test_signal", "off")
        a.wr("input_mux", saved["input_mux"])
        a.wr("hardwaregain", saved["hardwaregain"])
        a.wr("sampling_frequency", saved["sampling_frequency"])
        print(f"\n# restaurado: {', '.join(f'{k}={a.rd(k)}' for k in saved)}")


if __name__ == "__main__":
    main()
