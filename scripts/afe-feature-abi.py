#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# O ti-ads1299 da branch que vai para a mainline (feature/ads1299-mcp2210-driver)
# contra o silicio, pela ABI dele: atributos por canal, in_temp_*, os dois MVDD,
# lead-off por canal, e as recusas (-EINVAL, -EBUSY). O afe-phase4.py e o
# afe-phase5-long.py continuam sendo os da board/medplatform-afe, cuja ABI e
# outra (input_mux e hardwaregain por dispositivo, temperature_channel).
#
# Roda no PC, como root, com a ponte da board e o conversor da feature
# carregados (ROTEIRO_BANCADA_AFE.md §4, com MCP2210_REF/ADS1299_REF):
#
#   sudo python3 scripts/afe-feature-abi.py
#   sudo python3 scripts/afe-feature-abi.py --only buffer --seconds 20
#
# Chave SRB1 da placa: em GNDA durante todo o roteiro. O montage e escrito e
# relido, e volta ao valor encontrado; com as entradas abertas o sinal nao diz
# nada sobre o montage (4,7 nF entre P e N, sem caminho DC), entao aqui ele e
# so um registrador.
#
# Nao testado aqui, porque o PC nao tem: o evento openwire (o 6.8 nao tem o
# tipo; o build do host o tira), DRDY como interrupcao, saida de clock,
# gpiochip, PWDN e suspend.
#
# Valores esperados de BRINGUP_AFE.md: gerador VREF/2400 = 1,875 mV (§4.3),
# 83886 codigos ABAIXO do offset no ganho 24 em DC; MVDD do CH1 2,497 V com AVDD
# 5,00 V (§5.3); temperatura ~34 C no ar do laboratorio.
#
# Deixa o dispositivo como encontrou.

import argparse
import errno
import glob
import os
import select
import struct
import time

VREF_MV = 4500.0
GEN_MV = VREF_MV / 2400            # 1,875 mV (BRINGUP_AFE.md §4.3)
GAINS = (1, 2, 4, 6, 8, 12, 24)

results = []


def check(ok, msg):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {msg}")


def find_dev():
    for d in sorted(glob.glob("/sys/bus/iio/devices/iio:device*")):
        try:
            if open(f"{d}/name").read().strip().startswith("ads1299"):
                return d
        except OSError:
            pass
    raise SystemExit("ERRO: nenhum iio:device ads1299-*. O probe passou? (dmesg | grep ads1299)")


class Afe:
    def __init__(self, d):
        self.d = d
        self.node = "/dev/" + os.path.basename(d)
        self.nch = int(open(f"{d}/name").read().strip().split("-")[1])

    def rd(self, a):
        with open(f"{self.d}/{a}") as f:
            return f.read().strip()

    def wr(self, a, v):
        with open(f"{self.d}/{a}", "w") as f:
            f.write(str(v))

    def refused(self, a, v, err):
        """True quando escrever v em a falha com err."""
        try:
            self.wr(a, v)
        except OSError as e:
            return e.errno == err
        return False

    def read_refused(self, a, err):
        try:
            self.rd(a)
        except OSError as e:
            return e.errno == err
        return False

    def raw(self, ch, n):
        return [int(self.rd(f"in_voltage{ch}_raw")) for _ in range(n)]

    def mean(self, ch, n):
        v = self.raw(ch, n)
        return sum(v) / len(v), max(v) - min(v)

    def mux(self, ch, v=None):
        k = f"in_voltage{ch}_input_mux"
        return self.rd(k) if v is None else self.wr(k, v)

    def gain(self, ch, v=None):
        k = f"in_voltage{ch}_hardwaregain"
        return int(float(self.rd(k))) if v is None else self.wr(k, v)

    def scale_mv(self, ch):
        return float(self.rd(f"in_voltage{ch}_scale"))


# ---------------------------------------------------------------- estado

PER_CH = ("input_mux", "hardwaregain", "srb2", "lead_off_sense_p",
          "lead_off_sense_n", "lead_off_flip", "bias_sense_p", "bias_sense_n")
PER_DEV = ("sampling_frequency", "test_signal", "montage", "bias_drive",
           "bias_lead_off", "lead_off_threshold_percent",
           "lead_off_current_nanoamp", "lead_off_frequency")


def save(a):
    s = {k: a.rd(k) for k in PER_DEV}
    for ch in range(a.nch):
        for k in PER_CH:
            s[f"in_voltage{ch}_{k}"] = a.rd(f"in_voltage{ch}_{k}")
    return s


def restore(a, s):
    try:
        a.wr("buffer0/enable", 0)
    except OSError:
        pass
    # MUX antes do gerador: "off" e recusado com um canal na entrada de teste
    for ch in range(a.nch):
        k = f"in_voltage{ch}_input_mux"
        if s[k] != "test_signal":
            a.wr(k, s[k])
    a.wr("test_signal", s["test_signal"])
    for k, v in s.items():
        if k == "test_signal":
            continue
        try:
            a.wr(k, v)
        except OSError as e:
            print(f"  aviso: nao restaurou {k}={v}: {e}")


def baseline(a):
    """Todos os canais nas entradas, gerador desligado, sem lead-off, ganho 24."""
    for ch in range(a.nch):
        a.mux(ch, "normal")
        a.gain(ch, 24)
        for k in ("lead_off_sense_p", "lead_off_sense_n", "lead_off_flip"):
            a.wr(f"in_voltage{ch}_{k}", 0)
    a.wr("test_signal", "off")
    a.wr("lead_off_frequency", "dc")


# ---------------------------------------------------------------- secoes

def sec_probe(a):
    print("\n== Probe e identidade")
    check(a.rd("timestamp_source") == "host_timer",
          f"timestamp_source = {a.rd('timestamp_source')} (sem DRDY pela USB: host_timer)")
    mux_av = a.rd("in_voltage_input_mux_available").split()
    check("test_signal" in mux_av and "temperature" in mux_av,
          f"in_voltage_input_mux_available: {' '.join(mux_av)}")
    g = [int(float(x)) for x in a.rd("in_voltage0_hardwaregain_available").split()]
    check(tuple(g) == GAINS, f"ganhos disponiveis: {g}")
    print(f"  info  taxas: {a.rd('sampling_frequency_available')}")


def sec_generator(a, n):
    print(f"\n== Gerador em DC, os 7 ganhos, canal 0 ({n} leituras por ponto)")
    print(f"   esperado: nivel - offset = -{GEN_MV:.4f} mV (1x_dc) e -{2 * GEN_MV:.4f} mV (2x_dc);")
    print("   em DC o gerador fica no nivel BAIXO, abaixo do offset (o autoteste do driver conta com isso)")
    for name, mult in (("1x_dc", 1), ("2x_dc", 2)):
        vals = []
        for g in GAINS:
            a.gain(0, g)
            a.mux(0, "shorted")
            off, _ = a.mean(0, n)
            a.wr("test_signal", name)
            a.mux(0, "test_signal")
            lvl, pp = a.mean(0, n)
            mv = (lvl - off) * a.scale_mv(0)
            vals.append(mv)
            print(f"  {name} ganho {g:>2}: offset {off:>10.0f}  nivel {lvl:>10.0f} (pp {pp:>4})  -> {mv:+.4f} mV")
            a.mux(0, "shorted")
            a.wr("test_signal", "off")
        avg = sum(vals) / len(vals)
        spread = (max(vals) - min(vals)) / avg * 100
        err = (-avg - mult * GEN_MV) / (mult * GEN_MV) * 100
        check(spread <= 1.0, f"{name}: dispersao entre ganhos {spread:.2f}% (criterio 1%)")
        check(abs(err) <= 5.0, f"{name}: media {err:+.2f}% do esperado (criterio 5%)")
    a.mux(0, "normal")


def sec_per_channel(a, n):
    print("\n== Independencia por canal: MUX e ganho")
    a.wr("test_signal", "1x_dc")
    a.mux(0, "shorted")
    a.mux(1, "test_signal")
    a.gain(0, 24)
    a.gain(1, 6)
    check(a.mux(0) == "shorted" and a.mux(1) == "test_signal",
          f"MUX relido: ch0 {a.mux(0)}, ch1 {a.mux(1)}")
    check(a.gain(0) == 24 and a.gain(1) == 6, f"ganho relido: ch0 {a.gain(0)}, ch1 {a.gain(1)}")
    # o sysfs da 9 decimais: no ganho 24 sao 5 algarismos (0.000022351), e a
    # razao herda esse arredondamento - 1e-9 / 2.2e-5, ~5e-5 relativo por escala
    r = a.scale_mv(1) / a.scale_mv(0)
    check(abs(r - 4) / 4 < 2e-4, f"scale ch1 / scale ch0 = {r:.6f} (24/6 = 4, dentro do arredondamento do sysfs)")
    off0, _ = a.mean(0, n)
    lvl1, _ = a.mean(1, n)
    a.mux(1, "shorted")
    off1, _ = a.mean(1, n)
    mv1 = (lvl1 - off1) * a.scale_mv(1)
    check(abs(-mv1 - GEN_MV) / GEN_MV <= 0.05,
          f"ch1 no gerador, ganho 6: {mv1:+.4f} mV (esperado -{GEN_MV:.4f})")
    a.wr("test_signal", "2x_dc")
    moved, _ = a.mean(0, n)
    check(abs(moved - off0) < 2000,
          f"ch0 em curto nao seguiu o gerador: {off0:.0f} -> {moved:.0f} codigos")
    a.wr("test_signal", "off")
    a.mux(0, "normal")
    a.mux(1, "normal")
    a.gain(1, 24)


def sec_refusals(a):
    print("\n== Recusas sem buffer (-EINVAL)")
    a.wr("test_signal", "off")
    check(a.refused("in_voltage0_input_mux", "test_signal", errno.EINVAL),
          "entrada de teste com o gerador desligado: -EINVAL")
    a.wr("test_signal", "1x_dc")
    a.mux(0, "test_signal")
    check(a.refused("test_signal", "off", errno.EINVAL),
          "gerador 'off' com um canal na entrada de teste: -EINVAL")
    a.mux(0, "normal")
    a.wr("test_signal", "off")
    check(a.refused("in_voltage0_hardwaregain", 3, errno.EINVAL), "ganho 3: -EINVAL")
    check(a.refused("sampling_frequency", 300, errno.EINVAL), "taxa 300: -EINVAL")


def sec_internal(a, n, avdd):
    print("\n== Temperatura e alimentacoes (leitura direta, canal emprestado)")
    before = (a.mux(0), a.gain(0), a.mux(2), a.gain(2))
    t = [(int(a.rd("in_temp_raw")) + float(a.rd("in_temp_offset"))) * float(a.rd("in_temp_scale"))
         for _ in range(n)]
    c = sum(t) / len(t) / 1000
    check(15 <= c <= 60, f"in_temp: {c:.2f} C (plausivel 15..60; BRINGUP_AFE.md §5.3 ~34 C)")
    an = sum(int(a.rd("in_voltage_mvdd_analog_raw")) for _ in range(n)) / n
    va = an * float(a.rd("in_voltage_mvdd_analog_scale")) / 1000
    dg = sum(int(a.rd("in_voltage_mvdd_digital_raw")) for _ in range(n)) / n
    vd = dg * float(a.rd("in_voltage_mvdd_digital_scale")) / 1000
    if avdd:
        e = (va - avdd / 2) / (avdd / 2) * 100
        check(abs(e) <= 2, f"mvdd_analog {va:.4f} V contra AVDD/2 = {avdd / 2:.4f} V: {e:+.2f}% (2%)")
    else:
        check(abs(va - 2.497) <= 0.05, f"mvdd_analog {va:.4f} V (BRINGUP_AFE.md §5.3: 2,497 V)")
    print(f"  info  mvdd_digital {vd:.4f} V; se for DVDD/4, DVDD = {4 * vd:.3f} V (conferir no multimetro)")
    after = (a.mux(0), a.gain(0), a.mux(2), a.gain(2))
    check(before == after, f"canais 0 e 2 devolvidos como estavam: {after}")


def sec_lead_off(a):
    print("\n== Lead-off, DC: so entradas monitoradas aparecem")
    a.wr("lead_off_frequency", "dc")
    check(a.rd("lead_off_status") == "disabled",
          f"nada monitorado: lead_off_status = {a.rd('lead_off_status')}")
    a.wr("in_voltage0_lead_off_sense_p", 1)
    time.sleep(0.1)
    st = a.rd("lead_off_status")
    p, nn = (int(x, 16) for x in st.split())
    check(p & ~0x01 == 0 and nn == 0,
          f"so o ch1 P monitorado: '{st}' (bits fora do ch1 P tem de ser 0)")
    print(f"  info  ch1 P {'fora' if p & 1 else 'conectado'} (entrada aberta tende a 'fora')")
    a.wr("in_voltage0_lead_off_sense_n", 1)
    a.wr("in_voltage0_lead_off_flip", 1)
    time.sleep(0.1)
    st = a.rd("lead_off_status")
    p, nn = (int(x, 16) for x in st.split())
    check(p & ~0x01 == 0 and nn & ~0x01 == 0, f"ch1 P e N, flip: '{st}'")
    for k, v in (("lead_off_threshold_percent", "70"), ("lead_off_current_nanoamp", "24"),
                 ("lead_off_frequency", "ac_31.2")):
        a.wr(k, v)
        check(a.rd(k) == v, f"{k} = {a.rd(k)}")
    check(a.rd("lead_off_status") == "ac", f"AC: lead_off_status = {a.rd('lead_off_status')}")
    a.wr("lead_off_frequency", "dc")
    a.wr("bias_lead_off", "on")
    print(f"  info  bias_lead_off_status = {a.rd('bias_lead_off_status')}")
    a.wr("bias_lead_off", "off")
    check(a.rd("bias_lead_off_status") == "disabled",
          f"bias_lead_off off: {a.rd('bias_lead_off_status')}")
    for k in ("lead_off_sense_p", "lead_off_sense_n", "lead_off_flip"):
        a.wr(f"in_voltage0_{k}", 0)


def sec_switches(a):
    print("\n== SRB1 (montage), SRB2, bias - escrita e releitura; chave SRB1 em GNDA")
    m = a.rd("montage")
    for v in a.rd("montage_available").split():
        a.wr("montage", v)
        check(a.rd("montage") == v, f"montage = {a.rd('montage')}")
    a.wr("montage", m)
    a.wr("in_voltage3_srb2", 1)
    check(a.rd("in_voltage3_srb2") == "1" and a.rd("in_voltage2_srb2") == "0",
          "srb2 so no ch4")
    a.wr("in_voltage3_srb2", 0)
    a.wr("in_voltage0_bias_sense_p", 1)
    a.wr("in_voltage0_bias_sense_n", 1)
    a.wr("bias_drive", "on")
    check(a.rd("bias_drive") == "on", f"bias_drive = {a.rd('bias_drive')}")
    a.wr("bias_drive", "off")
    a.wr("in_voltage0_bias_sense_p", 0)
    a.wr("in_voltage0_bias_sense_n", 0)


def scan_layout(a):
    """(nome, offset, struct-fmt, shift, bits) dos elementos ligados, pelo _index/_type."""
    b = f"{a.d}/buffer0"
    els = []
    for en in glob.glob(f"{b}/in_*_en"):
        if open(en).read().strip() != "1":
            continue
        name = os.path.basename(en)[:-3]
        idx = int(open(f"{b}/{name}_index").read())
        t = open(f"{b}/{name}_type").read().strip()      # ex. le:s24/32>>0
        endian, rest = t.split(":")
        sign, rest = rest[0], rest[1:]
        bits, rest = rest.split("/")
        store, shift = rest.split(">>")
        els.append((idx, name, endian, sign, int(bits), int(store), int(shift)))
    els.sort()
    off, out = 0, []
    for _, name, endian, sign, bits, store, shift in els:
        size = store // 8
        off = (off + size - 1) // size * size
        code = {1: "B", 2: "H", 4: "I", 8: "Q"}[size]
        out.append((name, off, ("<" if endian == "le" else ">") + code, sign, bits, shift))
        off += size
    maxsz = max(struct.calcsize(f[2]) for f in out)
    return out, (off + maxsz - 1) // maxsz * maxsz


def read_buffer(a, seconds):
    b = f"{a.d}/buffer0"
    for f in glob.glob(f"{b}/in_*_en"):
        open(f, "w").write("1")
    open(f"{b}/length", "w").write("4096")
    layout, size = scan_layout(a)
    fd = os.open(a.node, os.O_RDONLY | os.O_NONBLOCK)
    rows, buf = [], b""
    try:
        a.wr("buffer0/enable", 1)
        busy = during_buffer(a)
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if select.select([fd], [], [], 0.2)[0]:
                try:
                    buf += os.read(fd, 8192)
                except BlockingIOError:
                    pass
            while len(buf) >= size:
                r = {}
                for name, off, fmt, sign, bits, shift in layout:
                    v = struct.unpack_from(fmt, buf, off)[0] >> shift
                    v &= (1 << bits) - 1
                    if sign == "s" and v & (1 << (bits - 1)):
                        v -= 1 << bits
                    r[name] = v
                rows.append(r)
                buf = buf[size:]
        lost = int(a.rd("lost_samples"))
    finally:
        a.wr("buffer0/enable", 0)
        os.close(fd)
    return layout, size, rows, lost, busy


def during_buffer(a):
    """As recusas com o buffer ligado; roda logo depois do enable."""
    return [
        (a.refused("in_voltage0_hardwaregain", 12, errno.EBUSY), "hardwaregain"),
        (a.refused("in_voltage0_input_mux", "shorted", errno.EBUSY), "input_mux"),
        (a.refused("test_signal", "2x_slow", errno.EBUSY), "test_signal"),
        (a.refused("sampling_frequency", 500, errno.EBUSY), "sampling_frequency"),
        (a.refused("montage", "referential", errno.EBUSY), "montage"),
        (a.refused("in_voltage1_lead_off_sense_p", 1, errno.EBUSY), "lead_off_sense_p"),
        (a.read_refused("in_voltage_mvdd_analog_raw", errno.EBUSY), "leitura de mvdd_analog"),
        (a.read_refused("in_temp_raw", errno.EBUSY), "leitura de in_temp"),
    ]


def sec_buffer(a, seconds):
    rate = 250
    print(f"\n== Buffer, {seconds:.0f} s a {rate} SPS: gerador 1x_slow no ch1, lead-off do ch1 P")
    a.wr("sampling_frequency", rate)
    a.wr("test_signal", "1x_slow")
    a.mux(0, "test_signal")
    a.wr("in_voltage0_lead_off_sense_p", 1)
    layout, size, rows, lost, busy = read_buffer(a, seconds)
    print("  info  layout: " + ", ".join(f"{n}@{o}" for n, o, *_ in layout) + f"; {size} bytes")
    for ok, what in busy:
        check(ok, f"com o buffer ligado, {what}: -EBUSY")
    if len(rows) < 20:
        check(False, f"so {len(rows)} amostras")
        return
    ts = [r["in_timestamp"] for r in rows]
    span = (ts[-1] - ts[0]) / 1e9
    due = round(span * rate)
    check(abs(len(rows) + lost - due) <= max(3, due * 0.002),
          f"{len(rows)} entregues + {lost} perdidas = {len(rows) + lost}, devidas ~{due} em {span:.2f} s")
    print(f"  info  taxa entregue {len(rows) / span:.1f}/s (a ponte entrega 250/s com folga zero, BRINGUP_AFE.md §6)")
    v = sorted(r["in_voltage0"] for r in rows)
    lo, hi = v[len(v) // 50], v[-len(v) // 50 - 1]
    pp = (hi - lo) * a.scale_mv(0)
    check(abs(pp - 2 * GEN_MV) / (2 * GEN_MV) <= 0.05,
          f"ch1, 1x_slow: pico a pico {pp:.4f} mV (esperado {2 * GEN_MV:.4f})")
    lk = [k for k in rows[0] if k.endswith("_lead_off")]
    if lk:
        words = {r[lk[0]] for r in rows}
        check(all(w & ~0x0100 == 0 for w in words),
              f"{lk[0]}: valores {sorted(hex(w) for w in words)} (so o bit do ch1 P pode aparecer)")
    else:
        check(False, "nenhum elemento *_lead_off no buffer")
    a.wr("in_voltage0_lead_off_sense_p", 0)
    a.mux(0, "normal")
    a.wr("test_signal", "off")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--avdd", type=float, help="AVDD medido no multimetro, em volts")
    ap.add_argument("-n", type=int, default=20, help="leituras avulsas por ponto")
    ap.add_argument("--seconds", type=float, default=10.0, help="janela do buffer")
    ap.add_argument("--only", choices=("probe", "generator", "channels", "refusals",
                                       "internal", "leadoff", "switches", "buffer"))
    args = ap.parse_args()

    a = Afe(find_dev())
    saved = save(a)
    print(f"# {a.d} ({a.rd('name')}), {a.rd('sampling_frequency')} SPS, "
          f"timestamp {a.rd('timestamp_source')}; chave SRB1 em GNDA")
    try:
        baseline(a)
        run = lambda k: args.only in (None, k)
        if run("probe"):
            sec_probe(a)
        if run("generator"):
            sec_generator(a, args.n)
        if run("channels"):
            sec_per_channel(a, args.n)
        if run("refusals"):
            sec_refusals(a)
        if run("internal"):
            sec_internal(a, args.n, args.avdd)
        if run("leadoff"):
            sec_lead_off(a)
        if run("switches"):
            sec_switches(a)
        if run("buffer"):
            sec_buffer(a, args.seconds)
    finally:
        restore(a, saved)
        changed = {k: v for k, v in save(a).items() if saved[k] != v}
        print(f"\n# restaurado{'' if not changed else ' MENOS: ' + str(changed)}")
    print(f"# {sum(results)} PASS, {len(results) - sum(results)} FAIL")
    raise SystemExit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
