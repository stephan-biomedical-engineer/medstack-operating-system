#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host side of the AMP bench (implementation_plan_m33_firmware.md, phase 3).

The board has no Python, so the bench is split: this script writes the
control messages the board sends with `cat`, and analyses what the board
recorded with `dd`. The board side is scripts/amp-bench.sh.

    amp-bench.py control OUT.bin [key=value ...]   a ControlMessage, as
                                                   encodeControl() builds it
    amp-bench.py analyse CAPTURE [--seconds S]     verify a synthetic capture
    amp-bench.py converter CAPTURE [--seconds S]   verify a capture of the real
                                                   converter's internal test signal
    amp-bench.py leadoff CAPTURE                   a capture on the electrode inputs:
                                                   integrity, and what lead-off said

Everything here is written from med_amp_abi.h and med_producer.h, NOT from the
framework's code, so that it is a second reading of the format and not the
first one run twice. The CRC is zlib's, which is CRC-32/IEEE 802.3 - the same
polynomial, reflection and final XOR the header specifies, from a source
outside this repository.
"""

import math
import struct
import sys
import zlib

CONTROL_MAGIC = 0x4D435452
ACK_MAGIC = 0x4D435441
FRAME_MAGIC = 0x4D454547
CONTROL = struct.Struct('<IHHII')     # magic version optionCount crc32 reserved
OPTION = 56                           # key[32] value[24]
ACK = struct.Struct('<IHH64s')        # magic version rejectedIndex detail
FRAME = struct.Struct('<IHHIIQQiI')   # 40 bytes
LEAD_OFF = struct.Struct('<HHHH')     # v2: monitored P, N; off P, N - after the samples
FRAME_VERSIONS = (1, 2)               # v1 captures predate lead-off and stay readable


def trailer(version):
    return LEAD_OFF.size if version >= 2 else 0
assert CONTROL.size == 16 and ACK.size == 72 and FRAME.size == 40 and LEAD_OFF.size == 8


def control(pairs):
    options = b''
    for key, value in pairs:
        k, v = key.encode(), value.encode()
        if len(k) >= 32 or len(v) >= 24:
            raise SystemExit('option does not fit: %s=%s' % (key, value))
        options += k.ljust(32, b'\0') + v.ljust(24, b'\0')
    crc = zlib.crc32(options) & 0xFFFFFFFF
    body = CONTROL.pack(CONTROL_MAGIC, 1, len(pairs), crc, 0) + options
    return body.ljust(CONTROL.size + 8 * OPTION, b'\0')


def expected_nv(n, c, internal=False, gain=24.0, ref_uv=4500000.0):
    """The synthetic signal from its specification (med_producer.c), double
    precision. Compared with a tolerance: the M33's sinf is newlib's."""
    full = ref_uv / gain * 1000.0
    if internal:
        a = ref_uv / 2400.0 * 1000.0
        v = a if n % 256 < 128 else -a
    else:
        t = n / 250.0
        v = 20000.0 * math.sin(2 * math.pi * (10 * t + c / 8.0)) + \
            5000.0 * math.sin(2 * math.pi * 50 * t)
    return max(-full, min(full, v))


def analyse(path, seconds=None, internal=False):
    data = open(path, 'rb').read()
    off, frames, acks, junk = 0, [], [], 0
    while off + 4 <= len(data):
        magic, = struct.unpack_from('<I', data, off)
        if magic == ACK_MAGIC and off + ACK.size <= len(data):
            m, ver, rej, detail = ACK.unpack_from(data, off)
            acks.append((off, ver, rej, detail.split(b'\0')[0].decode(errors='replace')))
            off += ACK.size
        elif magic == FRAME_MAGIC and off + FRAME.size <= len(data):
            h = FRAME.unpack_from(data, off)
            n = h[2] * h[3] * 4
            body = data[off + FRAME.size: off + FRAME.size + n + trailer(h[1])]
            frames.append((off, h, body))
            off += FRAME.size + n + trailer(h[1])
        else:
            junk += 1
            off += 1

    print('captura: %d bytes, %d ack(s), %d quadro(s), %d byte(s) sem dono'
          % (len(data), len(acks), len(frames), junk))
    for o, ver, rej, detail in acks:
        print('  ack @%d: versão %d, rejectedIndex %d, "%s"' % (o, ver, rej, detail))
    if acks and frames and frames[0][0] < acks[0][0]:
        print('  ATENÇÃO: há quadro ANTES do ack - o receptor tomaria o quadro pelo ack')
    if not frames:
        return 1

    bad_crc = bad_geom = bad_sample = gaps = 0
    worst = 0.0
    prev_seq = None
    stamps = []
    for o, h, body in frames:
        magic, ver, ch, spc, rate_mhz, seq, ts, scale, crc = h
        if (ch, spc, rate_mhz, scale) != (8, 14, 250000, 1) or ver not in FRAME_VERSIONS:
            bad_geom += 1
        if zlib.crc32(body) & 0xFFFFFFFF != crc:
            bad_crc += 1
            continue
        payload = body[:ch * spc * 4]
        if prev_seq is not None and seq != prev_seq + 1:
            gaps += 1
        prev_seq = seq
        stamps.append((seq, ts))
        samples = struct.unpack('<%di' % (ch * spc), payload)
        for i in range(spc):
            for c in range(ch):
                e = abs(samples[i * ch + c] - expected_nv(seq * spc + i, c, internal))
                worst = max(worst, e)
                if e > 3.0:
                    bad_sample += 1

    first, last = stamps[0], stamps[-1]
    span_us = last[1] - first[1]
    span_frames = last[0] - first[0]
    print('quadros: sequência %d..%d, lacunas %d, CRC errado %d, geometria errada %d'
          % (first[0], last[0], gaps, bad_crc, bad_geom))
    print('amostras: %d fora de 3 nV do sinal especificado (pior: %.2f nV)'
          % (bad_sample, worst))
    if span_frames:
        per_frame = span_us / span_frames
        deltas = [b[1] - a[1] for a, b in zip(stamps, stamps[1:]) if b[0] == a[0] + 1]
        print('carimbo do M33: %.3f us/quadro (esperado 56000), delta min %d max %d us'
              % (per_frame, min(deltas), max(deltas)))
        print('  -> taxa pelo relógio do M33: %.4f amostras/s/canal'
              % (span_frames * 14 / (span_us / 1e6)))
    if seconds:
        print('  -> taxa pelo relógio do LINUX (%.2f s de captura): %.3f amostras/s/canal'
              % (seconds, len(frames) * 14 / seconds))
    ok = gaps == 0 and bad_crc == 0 and bad_geom == 0 and bad_sample == 0
    print('VEREDITO:', 'OK' if ok else 'FALHOU')
    return 0 if ok else 1


def parse(data):
    off, frames, acks = 0, [], []
    while off + 4 <= len(data):
        magic, = struct.unpack_from('<I', data, off)
        if magic == ACK_MAGIC and off + ACK.size <= len(data):
            m, ver, rej, detail = ACK.unpack_from(data, off)
            acks.append((ver, rej, detail.split(b'\0')[0].decode(errors='replace')))
            off += ACK.size
        elif magic == FRAME_MAGIC and off + FRAME.size <= len(data):
            h = FRAME.unpack_from(data, off)
            n = h[2] * h[3] * 4
            frames.append((h, data[off + FRAME.size: off + FRAME.size + n + trailer(h[1])]))
            off += FRAME.size + n + trailer(h[1])
        else:
            off += 1
    return acks, frames


def lead_off_summary(bodies):
    """What the lead-off blocks of a capture say: whether anything was
    monitored, and for each electrode in how many frames it was reported off.
    Only monitored electrodes are counted - an 'off' bit elsewhere is not a
    measurement (med_amp_abi.h)."""
    monitored = set()
    counts = {}
    if not bodies:
        print('lead-off: nenhum quadro v2 na captura (sem quadros, ou só v1, que não levava o estado dos eletrodos)')
        return
    for body in bodies:
        mp, mn, op, on = LEAD_OFF.unpack_from(body, len(body) - LEAD_OFF.size)
        monitored.add((mp, mn))
        for c in range(16):
            for sign, m, o in (('+', mp, op), ('-', mn, on)):
                if m >> c & 1 and o >> c & 1:
                    counts['%d%s' % (c + 1, sign)] = counts.get('%d%s' % (c + 1, sign), 0) + 1
    print('lead-off: monitorado %s' % ', '.join('P %04x N %04x' % m for m in sorted(monitored)))
    if counts:
        print('  soltos (quadros): ' + ' '.join('%s=%d' % kv for kv in sorted(counts.items(),
              key=lambda kv: (int(kv[0][:-1]), kv[0][-1]))))
    elif any(m != (0, 0) for m in monitored):
        print('  nenhum eletrodo monitorado foi reportado solto')


def converter(path, seconds=None):
    """The converter's internal test signal, judged against its specification
    and not against a formula: on every channel a square wave of +-VREF/2400
    (1.875 mV with the internal 4.5 V reference) at f_CLK/2^21, which is a
    period of 256 conversions whatever the oscillator's exact frequency. The
    amplitude tolerance is the kernel driver's self-test one, +-20%."""
    acks, frames = parse(open(path, 'rb').read())
    for ver, rej, detail in acks:
        print('ack: rejectedIndex %d, "%s"' % (rej, detail))
    if not frames:
        print('nenhum quadro'); return 1
    bad_crc = gaps = 0
    prev = None
    stamps, per_ch, v2_bodies = [], [[] for _ in range(8)], []
    for h, body in frames:
        magic, ver, ch, spc, rate, seq, ts, scale, crc = h
        if ver not in FRAME_VERSIONS or zlib.crc32(body) & 0xFFFFFFFF != crc:
            bad_crc += 1; continue
        if ver >= 2:
            v2_bodies.append(body)
        payload = body[:ch * spc * 4]
        if prev is not None and seq != prev + 1:
            gaps += 1
        prev = seq
        stamps.append((seq, ts))
        vals = struct.unpack('<%di' % (ch * spc), payload)
        for i in range(spc):
            for c in range(ch):
                per_ch[c].append(vals[i * ch + c] * scale)
    n = len(per_ch[0])
    print('quadros %d (%d amostras/canal), lacunas %d, CRC/versão errados %d' % (len(stamps), n, gaps, bad_crc))
    lead_off_summary(v2_bodies)
    if not n:
        print('VEREDITO: FALHOU (nenhum quadro válido)'); return 1
    ok = gaps == 0 and bad_crc == 0
    print('canal  baixo(uV)  alto(uV)  amplitude(uV)  offset(uV)  periodo(amostras)')
    for c, v in enumerate(per_ch):
        mid = (max(v) + min(v)) / 2.0
        hi = [x for x in v if x > mid]; lo = [x for x in v if x <= mid]
        h_m = sum(hi) / len(hi) / 1000.0; l_m = sum(lo) / len(lo) / 1000.0
        amp = (h_m - l_m) / 2.0; offs = (h_m + l_m) / 2.0
        edges = [i for i in range(1, len(v)) if (v[i] > mid) != (v[i - 1] > mid)]
        period = 2.0 * (edges[-1] - edges[0]) / (len(edges) - 1) if len(edges) > 1 else float('nan')
        good = abs(amp - 1875.0) <= 0.2 * 1875.0 and abs(period - 256.0) <= 2.0
        ok = ok and good
        print('  %d   %9.1f  %8.1f  %13.1f  %10.1f  %8.2f  %s' % (c, l_m, h_m, amp, offs, period,
              '' if good else '<- FORA'))
    if len(stamps) > 1:
        span = (stamps[-1][1] - stamps[0][1]) / 1e6
        nconv = (stamps[-1][0] - stamps[0][0]) * 14
        print('taxa pelo relógio do M33: %.4f conversões/s (nominal 250; o oscilador interno da peça contra o clock do M33)' % (nconv / span))
        d = [b[1] - a[1] for a, b in zip(stamps, stamps[1:]) if b[0] == a[0] + 1]
        print('intervalo entre quadros: min %d, max %d us (nominal 56000)' % (min(d), max(d)))
    if seconds:
        print('taxa pelo relógio do Linux (%.2f s): %.3f amostras/s/canal' % (seconds, n / seconds))
    print('VEREDITO:', 'OK' if ok else 'FALHOU')
    return 0 if ok else 1


def leadoff(path):
    """A capture with the channels on their electrode inputs: no waveform to
    judge, so only integrity and the lead-off blocks."""
    acks, frames = parse(open(path, 'rb').read())
    for ver, rej, detail in acks:
        print('ack: rejectedIndex %d, "%s"' % (rej, detail))
    good, bad, gaps, prev = [], 0, 0, None
    for h, body in frames:
        if h[1] not in FRAME_VERSIONS or zlib.crc32(body) & 0xFFFFFFFF != h[8]:
            bad += 1
            continue
        if prev is not None and h[5] != prev + 1:
            gaps += 1
        prev = h[5]
        if h[1] >= 2:
            good.append(body)
    print('quadros %d, CRC/versão errados %d, lacunas %d' % (len(frames), bad, gaps))
    lead_off_summary(good)
    return 0 if good and not bad and not gaps else 1


def main(argv):
    if len(argv) >= 3 and argv[1] == 'control':
        pairs = [a.split('=', 1) for a in argv[3:]]
        open(argv[2], 'wb').write(control(pairs))
        return 0
    if len(argv) >= 3 and argv[1] == 'converter':
        seconds = float(argv[argv.index('--seconds') + 1]) if '--seconds' in argv else None
        return converter(argv[2], seconds)
    if len(argv) >= 3 and argv[1] == 'leadoff':
        return leadoff(argv[2])
    if len(argv) >= 3 and argv[1] == 'analyse':
        seconds = float(argv[argv.index('--seconds') + 1]) if '--seconds' in argv else None
        return analyse(argv[2], seconds, '--internal' in argv)
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
