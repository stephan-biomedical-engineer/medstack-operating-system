#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# A tolerancia do autoteste do ti-ads1299 da serie, medida em vez de presumida.
#
# O autoteste do probe faz dois julgamentos, no estado que o probe deixa (ganho
# 24 em todo canal, 250 SPS), cada um sobre 10 conversoes single-shot depois
# de descartar 2, a 50 ms uma da outra:
#
#   1. entradas em curto: pico a pico <= ADS1299_SELF_TEST_NOISE_UV_PP (10 uV,
#      447 codigos a ganho 24);
#   2. gerador em DC menos o offset: gain * 2^23 / 2400 (83886 a ganho 24)
#      +- ADS1299_SELF_TEST_TOLERANCE_PCT (20 %).
#
# O probe so diz se passou e o maior ruido. Este roteiro mede as duas
# grandezas, por canal, muitas vezes:
#
#   sudo python3 scripts/afe-selftest-tolerance.py replicate --runs 100 --csv ~/st.csv
#       os dois passos repetidos pelo caminho do proprio driver: in_voltageN_raw
#       e o mesmo read_one single-shot. Diferenca: o autoteste tira os 8 canais
#       de uma conversao; aqui cada leitura e uma conversao de um canal.
#   sudo python3 scripts/afe-selftest-tolerance.py probes --runs 30
#       recarrega o ti_ads1299 e colhe o numero que o proprio probe imprime.
#   sudo python3 scripts/afe-selftest-tolerance.py replug --runs 10
#       voce desplugue e plugue; o probe roda com a placa saindo do zero. Exige
#       o hid_mcp2210 JA carregado com spi_chip_select=4 (um modulo carregado
#       guarda os parametros no replug).
#
# A tolerancia de mainline sai disto JUNTO com o datasheet (precisao da
# referencia, erro de ganho, ruido referido a entrada), que vale para toda
# peca; uma peca so diz quanto esta peca se afasta. Chave SRB1 em GNDA, nada
# no DB25. Deixa o dispositivo como encontrou.

import argparse
import re
import statistics
import subprocess
import sys
import time
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("abi", os.path.join(HERE, "afe-feature-abi.py"))
abi = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(abi)

GAIN = 24
READINGS, DISCARD, INTERVAL = 10, 2, 0.050        # os do driver
EXPECTED = GAIN * 2 ** 23 / 2400                   # 83886,08 codigos
CEILING = 10 * GAIN * 2 ** 23 / 4_500_000          # 447 codigos: 10 uV a ganho 24
UV_PER_CODE = 4_500_000 / (GAIN * 2 ** 23)         # 0,02235 uV


def stats(a, n):
    """Media e pico a pico por canal: DISCARD rodadas fora, READINGS dentro."""
    vals = [[] for _ in range(n)]
    for r in range(DISCARD + READINGS):
        row = [int(a.rd(f"in_voltage{ch}_raw")) for ch in range(n)]
        time.sleep(INTERVAL)
        if r >= DISCARD:
            for ch in range(n):
                vals[ch].append(row[ch])
    return [sum(v) / len(v) for v in vals], [max(v) - min(v) for v in vals]


def replicate(args):
    a = abi.Afe(abi.find_dev())
    saved = abi.save(a)
    n = a.nch
    noise, err = [[] for _ in range(n)], [[] for _ in range(n)]
    csv = open(args.csv, "w") if args.csv else None
    if csv:
        csv.write("run,canal,offset,nivel,amp,erro_pct,ruido_pp_codigos\n")
    print(f"# replicate: {args.runs} rodadas, ganho {GAIN}, 250 SPS, {READINGS} leituras "
          f"apos {DISCARD}; esperado {EXPECTED:.0f} codigos, teto de ruido {CEILING:.0f}")
    try:
        abi.baseline(a)                                   # ganho 24, MUX normal, gerador off
        a.wr("sampling_frequency", 250)
        t0 = time.monotonic()
        for run in range(args.runs):
            a.wr("test_signal", "off")
            for ch in range(n):
                a.mux(ch, "shorted")
            off, pp = stats(a, n)
            a.wr("test_signal", "1x_dc")
            for ch in range(n):
                a.mux(ch, "test_signal")
            lvl, _ = stats(a, n)
            for ch in range(n):
                a.mux(ch, "shorted")
            for ch in range(n):
                amp = abs(lvl[ch] - off[ch])
                e = (amp - EXPECTED) / EXPECTED * 100
                noise[ch].append(pp[ch])
                err[ch].append(e)
                if csv:
                    csv.write(f"{run},{ch + 1},{off[ch]:.1f},{lvl[ch]:.1f},{amp:.1f},"
                              f"{e:.4f},{pp[ch]}\n")
            if (run + 1) % 10 == 0 or run == 0:
                el = time.monotonic() - t0
                print(f"  {run + 1:>4}/{args.runs}  ruido max {max(max(x) for x in noise):>4} "
                      f"codigos  erro {min(min(x) for x in err):+.3f} a "
                      f"{max(max(x) for x in err):+.3f} %   ({el:.0f} s)")
    finally:
        if csv:
            csv.close()
        abi.restore(a, saved)

    print("\n== Ruido em curto, pico a pico de 10 leituras, por canal (codigos; uV a ganho 24)")
    for ch in range(n):
        v = sorted(noise[ch])
        print(f"  CH{ch + 1}: min {v[0]:>4}  mediana {statistics.median(v):>6.1f}  "
              f"p99 {v[int(0.99 * (len(v) - 1))]:>4}  max {v[-1]:>4}  "
              f"({v[-1] * UV_PER_CODE:.2f} uV)")
    allv = sorted(x for c in noise for x in c)
    print(f"  todos: max {allv[-1]} codigos = {allv[-1] * UV_PER_CODE:.2f} uV; "
          f"teto atual {CEILING:.0f} ({CEILING / allv[-1]:.1f}x o pior)")

    print(f"\n== Gerador em DC menos offset, erro contra {EXPECTED:.0f} codigos (%)")
    for ch in range(n):
        v = err[ch]
        sd = statistics.stdev(v) if len(v) > 1 else 0.0
        print(f"  CH{ch + 1}: media {statistics.mean(v):+.4f}  desvio {sd:.4f}  "
              f"min {min(v):+.4f}  max {max(v):+.4f}")
    alle = [x for c in err for x in c]
    print(f"  todos: {min(alle):+.4f} a {max(alle):+.4f} %; espalhamento entre canais "
          f"{max(statistics.mean(c) for c in err) - min(statistics.mean(c) for c in err):.4f} "
          f"pontos; tolerancia atual +-20 %")


def selftest_lines():
    out = subprocess.run(["dmesg"], capture_output=True, text=True).stdout.splitlines()
    return [ln for ln in out if "ads1299" in ln and ("self test" in ln)]


def wait_new_line(before, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        now = selftest_lines()
        if len(now) > before:
            return now[before:]
        time.sleep(0.3)
    return []


def report(vals, fails):
    if vals:
        v = sorted(vals)
        print(f"\n  {len(v)} probes: ruido min {v[0]}  mediana {statistics.median(v)}  "
              f"max {v[-1]} codigos ({v[-1] * UV_PER_CODE:.2f} uV); teto {CEILING:.0f}")
    print(f"  falhas: {len(fails)}")
    for ln in fails:
        print(f"    {ln}")


def probes(args):
    vals, fails = [], []
    print(f"# probes: recarrega o ti_ads1299 {args.runs} vezes (a ponte fica)")
    for i in range(args.runs):
        before = len(selftest_lines())
        subprocess.run(["modprobe", "-r", "ti_ads1299"], check=True)
        subprocess.run(["modprobe", "ti_ads1299"], check=True)
        new = wait_new_line(before, 15)
        if not new:
            raise SystemExit("ERRO: nenhum self test novo no dmesg em 15 s")
        for ln in new:
            m = re.search(r"noise up to (\d+) codes", ln)
            if m:
                vals.append(int(m.group(1)))
            else:
                fails.append(ln)
        print(f"  {i + 1:>3}/{args.runs}  {new[-1].split(']', 1)[1].strip()}")
    report(vals, fails)


def replug(args):
    vals, fails = [], []
    print(f"# replug: {args.runs} vezes. hid_mcp2210 tem de estar carregado com spi_chip_select=4")
    for i in range(args.runs):
        input(f"\n  {i + 1}/{args.runs}: DESPLUGUE a placa e tecle Enter ")
        time.sleep(1)
        before = len(selftest_lines())
        input("  agora PLUGUE e tecle Enter ")
        t = time.monotonic()
        new = wait_new_line(before, 30)
        if not new:
            print("  nenhum self test em 30 s - o hid_mcp2210 esta carregado com spi_chip_select=4?")
            continue
        for ln in new:
            m = re.search(r"noise up to (\d+) codes", ln)
            if m:
                vals.append(int(m.group(1)))
            else:
                fails.append(ln)
        print(f"  {new[-1].split(']', 1)[1].strip()}  ({time.monotonic() - t:.1f} s depois do Enter)")
    report(vals, fails)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("replicate", "probes", "replug"))
    ap.add_argument("--runs", type=int, default=100)
    ap.add_argument("--csv", help="replicate: uma linha por rodada e canal")
    args = ap.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("ERRO: rode com sudo")
    {"replicate": replicate, "probes": probes, "replug": replug}[args.mode](args)


if __name__ == "__main__":
    main()
