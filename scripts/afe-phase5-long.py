#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Fase 5 do implementation_plan_afe_bench.md: uma sessao longa pelo buffer.
#
# Contar amostras diz se elas chegaram; nao diz se chegaram CERTAS. Por isso a
# sessao roda com o gerador interno (1x_slow, ~1 Hz, +-VREF/2400) como sinal
# conhecido: toda amostra tem de estar num dos dois niveis da onda quadrada, ou
# numa transicao entre eles. Uma amostra fora dos dois niveis e cercada por
# vizinhas do MESMO nivel nao e transicao - e um quadro corrompido (bytes
# deslocados, uma resposta trocada pela ponte), que nenhum contador ve.
#
#   sudo python3 scripts/afe-phase5-long.py --minutes 30
#   sudo python3 scripts/afe-phase5-long.py --minutes 30 --csv ~/fase5-longa.csv
#
# Por janela de --window segundos: entregues contra devidas, lost_samples,
# intervalos entre timestamps (lacuna = mais de 1,5 periodo), e amostras
# corrompidas. No fim, o total e o pior caso. Os 8 canais vao para o buffer; a
# integridade e conferida em todos.
#
# Le so sysfs e /dev/iio:deviceN. Deixa o dispositivo como encontrou.

import argparse
import glob
import importlib.util
import os
import select
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("p4", os.path.join(HERE, "afe-phase4.py"))
p4 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(p4)

FMT, SIZE, NCH = "<8Iq", 40, 8


def levels(a, n=20):
    """Os dois niveis da onda, por canal, medidos antes da sessao.

    Pelo raw (single-shot) o gerador fica na fase 0, que e o nivel ALTO; o
    BAIXO e o simetrico em torno do offset em curto - medido em BRINGUP_AFE.md
    §4.3 como -84384 contra +82908 com offset -780, ou seja, simetrico.
    """
    hi, lo = [], []
    for ch in range(NCH):
        a.wr("test_signal", "off")
        a.wr("input_mux", "shorted")
        off = sum(a.raw(n, ch)) / n
        a.wr("test_signal", "1x_slow")
        a.wr("input_mux", "test_signal")
        h = sum(a.raw(n, ch)) / n
        hi.append(h)
        lo.append(2 * off - h)
    return hi, lo


def classify(v, hi, lo, band):
    """+1 no nivel alto, -1 no baixo, 0 fora dos dois."""
    if abs(v - hi) <= band:
        return 1
    if abs(v - lo) <= band:
        return -1
    return 0


def is_corrupt(before, mid, after, hi, lo, band):
    """A amostra do meio esta fora dos dois niveis E cercada pelo mesmo nivel.

    Uma transicao real passa de um nivel ao outro (vizinhas diferentes) ou
    ocupa varias amostras seguidas (vizinha tambem fora); so o valor isolado
    entre duas vizinhas iguais e um quadro errado.
    """
    if classify(mid, hi, lo, band) != 0:
        return False
    cb, ca = classify(before, hi, lo, band), classify(after, hi, lo, band)
    return cb != 0 and cb == ca


class Window:
    def __init__(self):
        self.n = 0
        self.gaps = 0
        self.max_dt = 0.0
        self.dts = []
        self.corrupt = 0
        self.t0 = None
        self.t1 = None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=30.0)
    ap.add_argument("--window", type=float, default=60.0, help="segundos por linha de relatorio")
    ap.add_argument("--csv", help="grava uma linha por janela neste arquivo")
    args = ap.parse_args()

    a = p4.Afe(p4.find_dev())
    saved = {k: a.rd(k) for k in ("hardwaregain", "input_mux", "test_signal", "sampling_frequency")}
    rate = int(a.rd("sampling_frequency"))
    period_ns = 1e9 / rate
    print(f"# Fase 5, sessao longa: {a.d} ({a.rd('name')}), {rate} SPS, ganho {a.rd('hardwaregain')}, "
          f"timestamp {a.rd('timestamp_source')}, {args.minutes:.0f} min em janelas de {args.window:.0f} s")

    hi, lo = levels(a)
    band = [abs(h - l) * 0.05 for h, l in zip(hi, lo)]   # 5% da excursao
    print("# niveis por canal (alto / baixo): " +
          "  ".join(f"CH{c + 1} {hi[c]:.0f}/{lo[c]:.0f}" for c in range(NCH)))

    a.wr("test_signal", "1x_slow")
    a.wr("input_mux", "test_signal")

    b = f"{a.d}/buffer0"
    for f in glob.glob(f"{b}/in_*_en"):
        open(f, "w").write("1")
    open(f"{b}/length", "w").write("8192")

    csv = open(args.csv, "w") if args.csv else None
    if csv:
        csv.write("janela,inicio_s,entregues,devidas,lost_samples_acum,lacunas,dt_max_ms,"
                  "dt_p99_ms,corrompidas\n")

    fd = os.open(a.node, os.O_RDONLY | os.O_NONBLOCK)
    tot = {"n": 0, "gaps": 0, "corrupt": 0, "max_dt": 0.0}
    prev_t = None
    hist = []                 # as ultimas 3 amostras, para julgar a do meio
    w = Window()
    wi = 0
    start = time.monotonic()
    end = start + args.minutes * 60
    buf = b""
    t_first = None
    try:
        open(f"{b}/enable", "w").write("1")
        next_report = start + args.window
        while True:
            now = time.monotonic()
            if select.select([fd], [], [], 0.2)[0]:
                try:
                    buf += os.read(fd, 65536)
                except BlockingIOError:
                    pass
            while len(buf) >= SIZE:
                f = struct.unpack(FMT, buf[:SIZE])
                buf = buf[SIZE:]
                t = f[8]
                vals = [p4.s24(x) for x in f[:NCH]]
                if t_first is None:
                    t_first = t
                if w.t0 is None:
                    w.t0 = t
                w.t1 = t
                w.n += 1
                if prev_t is not None:
                    dt = (t - prev_t) / 1e6
                    w.dts.append(dt)
                    w.max_dt = max(w.max_dt, dt)
                    if t - prev_t > 1.5 * period_ns:
                        w.gaps += 1
                prev_t = t
                hist.append(vals)
                if len(hist) == 3:
                    before, mid, after = hist
                    if any(is_corrupt(before[ch], mid[ch], after[ch], hi[ch], lo[ch], band[ch])
                           for ch in range(NCH)):
                        w.corrupt += 1
                    hist.pop(0)
            if now >= next_report or now >= end:
                wi += 1
                lost = int(a.rd("lost_samples"))
                span = (w.t1 - w.t0) / 1e9 if w.t0 is not None and w.t1 != w.t0 else 0
                due = round(span * rate) + 1 if span else 0
                p99 = sorted(w.dts)[int(0.99 * (len(w.dts) - 1))] if w.dts else 0
                line = (f"  [{wi:>3}] {(now - start):>6.0f} s: entregues {w.n:>6}  devidas ~{due:>6}  "
                        f"lost_samples(acum) {lost:>5}  lacunas {w.gaps:>4}  "
                        f"dt max {w.max_dt:>6.2f} ms p99 {p99:>5.2f}  corrompidas {w.corrupt}")
                print(line, flush=True)
                if csv:
                    csv.write(f"{wi},{now - start:.0f},{w.n},{due},{lost},{w.gaps},"
                              f"{w.max_dt:.3f},{p99:.3f},{w.corrupt}\n")
                    csv.flush()
                tot["n"] += w.n
                tot["gaps"] += w.gaps
                tot["corrupt"] += w.corrupt
                tot["max_dt"] = max(tot["max_dt"], w.max_dt)
                w = Window()
                next_report += args.window
                if now >= end:
                    break
    except KeyboardInterrupt:
        print("  interrompido - o resumo abaixo cobre ate aqui")
    finally:
        try:
            open(f"{b}/enable", "w").write("0")
        except OSError:
            pass
        os.close(fd)
        lost = int(a.rd("lost_samples"))
        a.wr("test_signal", "off")
        for k in ("input_mux", "hardwaregain", "sampling_frequency"):
            a.wr(k, saved[k])
        if csv:
            csv.close()

    span = (prev_t - t_first) / 1e9 if t_first is not None and prev_t is not None else 0
    due = round(span * rate) + 1 if span else 0
    print(f"\n== Resumo: {span / 60:.1f} min pelos timestamps do kernel")
    print(f"  entregues {tot['n']}   devidas ~{due}   lost_samples {lost}   "
          f"entregues + perdidas = {tot['n'] + lost}")
    if due:
        print(f"  perda contada pelo driver: {lost / due:.4%}   "
              f"diferenca nao contada: {due - tot['n'] - lost:+d} amostras")
    print(f"  lacunas (> 1,5 periodo): {tot['gaps']}   maior intervalo: {tot['max_dt']:.2f} ms")
    print(f"  {p4.verdict(tot['corrupt'] == 0)}  amostras corrompidas: {tot['corrupt']} "
          "(fora dos dois niveis, isoladas entre vizinhas iguais)")
    print(f"# restaurado: {', '.join(f'{k}={a.rd(k)}' for k in saved)}")


if __name__ == "__main__":
    main()
