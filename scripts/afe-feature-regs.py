#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Varredura de registradores do ti-ads1299 da serie (feature/ads1299-mcp2210-driver):
# o que cada ajuste da ABI deixa no SILICIO, bit a bit, contra a transcricao do
# datasheet (docs/Register_Map_ADS1299.md).
#
# Por que nao contra os valores de reset: o probe deste driver programa o
# conversor, entao o mapa depois dele nao e o de reset - e a comparacao com o
# reset ja foi feita pelo oraculo (BRINGUP_AFE.md §4.2).
#
# Como le o silicio. O regmap do driver tem cache, e a leitura dele devolve o
# cache. No Ubuntu com Secure Boot o kernel roda em lockdown "integrity", e o
# debugfs so abre arquivos somente-leitura: o cache_bypass do regmap e o
# direct_reg_access do IIO dao EPERM (2026-10-17). Entao, por padrao:
#
#   1. carrega os drivers, aplica um padrao pela ABI, le o cache do driver
#      (debugfs regmap/registers, 0400, que o lockdown deixa);
#   2. descarrega os drivers - que nao escrevem nada no conversor ao sair - e le
#      o silicio pelo oraculo (afe-spi-oracle.py), por hidraw, com um RREG que
#      nao e o do driver;
#   3. confere todo bit de todo registrador contra o que a transcricao preve
#      para aquele padrao, e o cache contra o silicio.
#
# Cada padrao varia tudo de uma vez - os 8 canais com ganhos e MUX diferentes,
# taxas, codigos de LOFF, mascaras - e nove padroes cobrem todo codigo de todo
# campo. O probe reseta o conversor, entao um padrao nao vaza para o seguinte.
# As posicoes abaixo foram escritas da transcricao, nao das macros do driver.
#
#   sudo python3 scripts/afe-feature-regs.py            # a varredura, ~1 min
#   sudo python3 scripts/afe-feature-regs.py --inject   # as injecoes tem de ser vistas
#
# Corrente de lead-off de 6 e 24 uA so e escrita com nenhuma entrada
# monitorada: nenhuma corrente flui. Chave SRB1 em GNDA, nada no DB25.
# Termina com o conversor resetado (RESET) e os drivers DESCARREGADOS, a ponte
# no hid-generic, pronta para o --compare da NVRAM.

import argparse
import glob
import importlib.util
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


abi = _load("abi", "afe-feature-abi.py")
orc = _load("orc", "afe-spi-oracle.py")

# --- a transcricao (Register_Map_ADS1299.md), e nada do driver ---------------
ID, CONFIG1, CONFIG2, CONFIG3, LOFF = 0x00, 0x01, 0x02, 0x03, 0x04
CHSET = lambda i: 0x05 + i                     # in_voltage(i) = CH(i+1)SET, 05h..0Ch
BIAS_SENSP, BIAS_SENSN, LOFF_SENSP, LOFF_SENSN, LOFF_FLIP = 0x0D, 0x0E, 0x0F, 0x10, 0x11
LOFF_STATP, LOFF_STATN, GPIO, MISC1, MISC2, CONFIG4 = 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
NREGS = 0x18
NAMES = ["ID", "CONFIG1", "CONFIG2", "CONFIG3", "LOFF"] + [f"CH{i}SET" for i in range(1, 9)] + \
        ["BIAS_SENSP", "BIAS_SENSN", "LOFF_SENSP", "LOFF_SENSN", "LOFF_FLIP",
         "LOFF_STATP", "LOFF_STATN", "GPIO", "MISC1", "MISC2", "CONFIG4"]

RATES = [16000, 8000, 4000, 2000, 1000, 500, 250]          # Tabela 13: DR = indice
GAINS = [1, 2, 4, 6, 8, 12, 24]                            # Tabela 17: GAIN = indice
MUXES = ["normal", "shorted", "bias_measurement", "supply",  # Tabela 17: MUX = indice
         "temperature", "test_signal", "bias_drive_p", "bias_drive_n"]
# Tabela 14: INT_CAL bit 4, CAL_AMP bit 2, CAL_FREQ 1:0 (00 lento, 01 rapido, 11 DC)
TEST = {"1x_slow": 0x10, "1x_fast": 0x11, "1x_dc": 0x13,
        "2x_slow": 0x14, "2x_fast": 0x15, "2x_dc": 0x17}
COMP_TH = ["95", "92.5", "90", "87.5", "85", "80", "75", "70"]   # Tabela 16, 7:5
ILEAD = ["6", "24", "6000", "24000"]                             # 3:2
FLEAD = ["dc", "ac_7.8", "ac_31.2", "ac_fdr_div_4"]              # 1:0

# Mudam sozinhos: fora de toda comparacao
STATUS = {CONFIG3: 0x01, LOFF_STATP: 0xFF, LOFF_STATN: 0xFF}
# O driver move por conta propria (leitura direta x buffer): PD de cada canal e
# SINGLE_SHOT. Fora da previsao; dentro da comparacao cache x silicio.
DRIVER_OWNED = {**{CHSET(i): 0x80 for i in range(8)}, CONFIG4: 0x08}

results = []


def check(ok, msg):
    results.append(ok)
    print(f"  {'PASS' if ok else 'FAIL'}  {msg}")
    return ok


def rot(x, k):
    k %= 8
    return ((x << k) | (x >> (8 - k))) & 0xFF


def pattern(k):
    """O padrao k: o que se escreve pela ABI. k = 8 e o estado de partida."""
    if k == 8:
        return dict(rate=250, test="off", th=0, il=0, fl=0, montage=0, bias=0, bias_loff=0,
                    gain=[24] * 8, mux=["normal"] * 8, srb2=[0] * 8,
                    bsp=0, bsn=0, lsp=0, lsn=0, flip=0)
    ua = k % 4 >= 2                               # 6 ou 24 uA: nada monitorado
    return dict(rate=RATES[k % 7], test=list(TEST)[k % 6], th=k, il=k % 4,
                fl=(k + k // 4) % 4, montage=(k >> 1) & 1, bias=k & 1,
                bias_loff=int(k in (1, 4)),
                gain=[GAINS[(i + k) % 7] for i in range(8)],
                mux=[MUXES[(i + k) % 8] for i in range(8)],
                srb2=[(i + k) % 2 for i in range(8)],
                bsp=rot(0x35, k), bsn=rot(0xA6, k),
                lsp=0 if ua else rot(0x19, k), lsn=0 if ua else rot(0x62, k),
                flip=rot(0x4B, k))


def expected(p):
    """O mapa que a transcricao preve para o padrao p."""
    m = [0] * NREGS
    m[ID] = 0x1E                                  # bit 4 = 1, DEV_ID 11, NU_CH 10; REV fora
    m[CONFIG1] = 0x90 | RATES.index(p["rate"])    # bit 7 = 1, 4:3 = 2h, DAISY_EN 0, CLK_EN 0
    m[CONFIG2] = 0xC0 | TEST.get(p["test"], 0)    # 7:5 = 6h, bit 3 = 0
    meas = "bias_measurement" in p["mux"]
    m[CONFIG3] = 0x80 | 0x60 | (0x10 if meas else 0) | (0x0C if p["bias"] else 0) | \
        (0x02 if p["bias_loff"] else 0)           # PD_REFBUF, 6:5 = 3h, BIASREF_INT com o bias
    m[LOFF] = (p["th"] << 5) | (p["il"] << 2) | p["fl"]
    for i in range(8):
        m[CHSET(i)] = (GAINS.index(p["gain"][i]) << 4) | (p["srb2"][i] << 3) | \
            MUXES.index(p["mux"][i])
    m[BIAS_SENSP], m[BIAS_SENSN] = p["bsp"], p["bsn"]
    m[LOFF_SENSP], m[LOFF_SENSN], m[LOFF_FLIP] = p["lsp"], p["lsn"], p["flip"]
    m[GPIO] = 0x0F                                # 4 entradas, pinos em 0 (pull-down)
    m[MISC1] = 0x20 if p["montage"] else 0x00
    m[CONFIG4] = 0x02 if (p["lsp"] or p["lsn"] or p["bias_loff"]) else 0x00
    return m


def care(r):
    return 0xFF & ~STATUS.get(r, 0) & ~DRIVER_OWNED.get(r, 0) & (0x1F if r == ID else 0xFF)


# --- os dois lados -----------------------------------------------------------

def wait(fn, what, timeout=20):
    end = time.monotonic() + timeout
    while True:
        try:
            return fn()
        except (SystemExit, OSError):
            if time.monotonic() > end:
                raise SystemExit(f"ERRO: {what} nao apareceu em {timeout} s")
            time.sleep(0.3)


def load_drivers():
    subprocess.run(["modprobe", "hid_mcp2210", "spi_chip_select=4", "led_gpio=6"], check=True)
    d = wait(abi.find_dev, "o iio:device do ads1299")
    log = subprocess.run(["dmesg"], capture_output=True, text=True).stdout.splitlines()
    st = [line for line in log if "ads1299" in line and "self test" in line]
    return abi.Afe(d), (st[-1].split("]", 1)[1].strip() if st else "?")


def unload_drivers():
    subprocess.run(["modprobe", "-r", "ti_ads1299", "hid_mcp2210"], check=True)
    return wait(orc.Bridge, "a ponte no hid-generic")


def driver_cache(a):
    spi = os.path.basename(os.path.dirname(os.path.realpath(a.d)))
    found = sorted(glob.glob(f"/sys/kernel/debug/regmap/{spi}*"))
    if not found:
        raise SystemExit(f"ERRO: nenhum regmap de {spi} em /sys/kernel/debug/regmap")
    m = {}
    with open(f"{found[0]}/registers") as f:
        for line in f:
            r, v = line.split(":")
            m[int(r, 16)] = int(v, 16)
    return [m[r] for r in range(NREGS)]


def silicon(b):
    b.xfer(bytes([orc.SDATAC]))
    time.sleep(0.01)
    return list(orc.rreg_all(b))


def apply(a, p):
    abi.baseline(a)                               # MUX normal, ganho 24, gerador off, lead-off off
    for ch in range(8):
        for k in ("srb2", "bias_sense_p", "bias_sense_n"):
            a.wr(f"in_voltage{ch}_{k}", 0)
    if p["test"] != "off":
        a.wr("test_signal", p["test"])            # antes do MUX de teste
    a.wr("sampling_frequency", p["rate"])
    a.wr("lead_off_threshold_percent", COMP_TH[p["th"]])
    a.wr("lead_off_current_nanoamp", ILEAD[p["il"]])
    a.wr("lead_off_frequency", FLEAD[p["fl"]])
    for ch in range(8):
        a.gain(ch, p["gain"][ch])
        a.mux(ch, p["mux"][ch])
        a.wr(f"in_voltage{ch}_srb2", p["srb2"][ch])
        for key, mask in (("bias_sense_p", "bsp"), ("bias_sense_n", "bsn"),
                          ("lead_off_sense_p", "lsp"), ("lead_off_sense_n", "lsn"),
                          ("lead_off_flip", "flip")):
            a.wr(f"in_voltage{ch}_{key}", (p[mask] >> ch) & 1)
    a.wr("montage", "referential" if p["montage"] else "bipolar")
    a.wr("bias_drive", "on" if p["bias"] else "off")
    a.wr("bias_lead_off", "on" if p["bias_loff"] else "off")


def compare(label, hw, cache, exp):
    bad = [f"{NAMES[r]} {hw[r]:02x} (previsto {exp[r]:02x} em {care(r):02x})"
           for r in range(NREGS) if hw[r] & care(r) != exp[r] & care(r)]
    check(not bad, f"{label}: silicio = transcricao" + (": " + "; ".join(bad) if bad else ""))
    div = [f"{NAMES[r]} cache {cache[r]:02x} silicio {hw[r]:02x}" for r in range(NREGS)
           if r != GPIO and cache[r] & ~STATUS.get(r, 0) != hw[r] & ~STATUS.get(r, 0)]
    check(not div, f"{label}: cache do driver = silicio" + (": " + "; ".join(div) if div else ""))
    return bad, div


def one(k):
    p = pattern(k)
    a, st = load_drivers()
    apply(a, p)
    cache = driver_cache(a)
    b = unload_drivers()
    try:
        hw = silicon(b)
    finally:
        b.close()
    print(f"\n== padrao {k}: {p['rate']} SPS, {p['test']}, LOFF {COMP_TH[p['th']]} %/"
          f"{ILEAD[p['il']]} nA/{FLEAD[p['fl']]}, montage {'ref' if p['montage'] else 'bip'}, "
          f"bias {p['bias']}, bias_lead_off {p['bias_loff']}")
    print(f"  info  probe: {st}")
    print(f"  info  silicio: {' '.join(f'{v:02x}' for v in hw)}")
    return p, hw, cache


def sweep():
    seen = {"rate": set(), "gain": set(), "mux": set(), "test": set(),
            "th": set(), "il": set(), "fl": set()}
    for k in list(range(8)) + [8]:
        p, hw, cache = one(k)
        compare(f"padrao {k}", hw, cache, expected(p))
        if k == 0:
            print(f"  info  REV_ID = {hw[ID] >> 5}")
        seen["rate"].add(p["rate"]); seen["test"].add(p["test"])
        seen["gain"].update(p["gain"]); seen["mux"].update(p["mux"])
        seen["th"].add(p["th"]); seen["il"].add(p["il"]); seen["fl"].add(p["fl"])
    print("\n== Cobertura")
    full = {"rate": 7, "gain": 7, "mux": 8, "test": 7, "th": 8, "il": 4, "fl": 4}
    for key, n in full.items():
        check(len(seen[key]) == n, f"{key}: {len(seen[key])} de {n} codigos")


def inject():
    print("== Injecoes: as comparacoes tem de reprovar")
    p, hw, cache = one(0)
    exp = expected(p)
    n = len(results)
    compare("controle (sem injecao)", hw, cache, exp)
    ok_control = all(results[n:])
    # 1. o silicio muda pelas costas do driver: o oraculo grava LOFF_FLIP ^ 01
    b = wait(orc.Bridge, "a ponte no hid-generic")
    try:
        orc.wreg(b, LOFF_FLIP, hw[LOFF_FLIP] ^ 0x01)
        hw2 = silicon(b)
    finally:
        b.close()
    n = len(results)
    bad, div = compare("injecao 1 (LOFF_FLIP ^ 01 pelo oraculo)", hw2, cache, exp)
    del results[n:]
    seen1 = any("LOFF_FLIP" in x for x in bad) and any("LOFF_FLIP" in x for x in div)
    # 2. posicao de campo errada: o ganho previsto um bit abaixo (5:3)
    wrong = list(exp)
    for i in range(8):
        wrong[CHSET(i)] = (exp[CHSET(i)] & 0x8F) | ((exp[CHSET(i)] >> 1) & 0x38)
    n = len(results)
    bad2, _ = compare("injecao 2 (ganho previsto em 5:3)", hw, cache, wrong)
    del results[n:]
    print()
    check(ok_control, "controle: o padrao 0 sem injecao passa")
    check(seen1, "injecao 1 vista: o silicio difere da previsao e do cache em LOFF_FLIP")
    check(bool(bad2), f"injecao 2 vista: {len(bad2)} CHnSET fora da previsao errada")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true", help="so as injecoes")
    args = ap.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("ERRO: rode com sudo (modprobe, debugfs, hidraw)")
    print("# varredura pelo oraculo: carrega, aplica, le o cache, descarrega, le o silicio")
    print("# chave SRB1 em GNDA, nada no DB25")
    try:
        inject() if args.inject else sweep()
    finally:
        try:
            subprocess.run(["modprobe", "-r", "ti_ads1299", "hid_mcp2210"], check=False,
                           stderr=subprocess.DEVNULL)
            b = wait(orc.Bridge, "a ponte no hid-generic", 10)
            b.xfer(bytes([orc.RESET]))
            b.close()
            print("\n# conversor resetado; drivers descarregados, ponte no hid-generic")
        except SystemExit as e:
            print(f"\n# aviso: nao resetou o conversor no fim ({e})")
    print(f"# {sum(results)} PASS, {len(results) - sum(results)} FAIL")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
