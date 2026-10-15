#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host checks of `amp-bench.py converter`, the judge of a test-signal record
(implementation_plan_usb_parity.md, phase 0.c).

The captures are synthesised here from the specification - a square wave of
+-VREF/2400 whose half period is rate * 2^20 / f_CLK samples - and written in
the frame layout of med_amp_abi.h, independently of the script under test.
Then conversions are taken away in the ways a link loses them, and the judge
has to say exactly how many. A judge that has never seen a loss is a claim.
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, '..', '..', 'scripts', 'amp-bench.py')
FRAME = struct.Struct('<IHHIIQQiI')
MAGIC, CH, SPC, AMP_NV = 0x4D454547, 8, 14, 1875000
checks = failures = 0


def square(rate, seconds, phase=37):
    """Sample values, nanovolts, one list per channel."""
    half = rate * 2 ** 20 // 2048000
    n = int(rate * seconds)
    return [[(AMP_NV if ((i + phase) // half) % 2 == 0 else -AMP_NV) + c * 1000
             for i in range(n)] for c in range(CH)]


def frames_of(chans, rate, renumber=True, drop_frames=()):
    """Cut the per-channel samples into frames; drop_frames removes whole frames
    after cutting, renumbering the sequence (renumber=True, a loss the sequence
    cannot see) or leaving the gap."""
    n = len(chans[0]) // SPC
    out, seq = [], 0
    for f in range(n):
        if f in drop_frames:
            if not renumber:
                seq += 1
            continue
        vals = [chans[c][f * SPC + i] for i in range(SPC) for c in range(CH)]
        body = struct.pack('<%di' % len(vals), *vals) + struct.pack('<HHHH', 0, 0, 0, 0)
        hdr = FRAME.pack(MAGIC, 2, CH, SPC, rate * 1000, seq, 1000000 + seq * SPC * 10 ** 6 // rate,
                         1, zlib.crc32(body) & 0xFFFFFFFF)
        out.append(hdr + body)
        seq += 1
    return b''.join(out)


def judge(blob):
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(blob)
    try:
        r = subprocess.run([sys.executable, SCRIPT, 'converter', f.name],
                           capture_output=True, text=True)
    finally:
        os.unlink(f.name)
    rows = [l.split() for l in r.stdout.splitlines() if l.startswith('  ') and l.split()[0].isdigit()]
    short = sum(int(x[7]) for x in rows)
    extra = sum(int(x[8]) for x in rows)
    gaps = int(r.stdout.split('lacunas ')[1].split(',')[0]) if 'lacunas ' in r.stdout else -1
    return r.returncode, short, extra, gaps, r.stdout


def check(name, cond, detail=''):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print('  FALHA: %s %s' % (name, detail))


print('amp-bench converter - verificações de host')

for rate, seconds in ((250, 20), (16000, 3)):
    code, short, extra, gaps, out = judge(frames_of(square(rate, seconds), rate))
    check('%d limpo: veredito OK' % rate, code == 0, out[-400:])
    check('%d limpo: nada faltando nem sobrando' % rate, short == 0 and extra == 0, (short, extra))

chans = square(1000, 10)
mid = len(chans[0]) // SPC // 2

# A frame the producer gave up and the sequence does not show: 14 missing on every channel.
code, short, extra, gaps, _ = judge(frames_of(chans, 1000, renumber=True, drop_frames={mid}))
check('quadro sumido, sequência contínua: FALHOU', code == 1)
check('quadro sumido: 14 faltando por canal', short == 14 * CH, short)
check('quadro sumido: a sequência não viu', gaps == 0, gaps)

# The same frame, with the gap left in the sequence: both judges see it.
code, short, extra, gaps, _ = judge(frames_of(chans, 1000, renumber=False, drop_frames={mid}))
check('quadro sumido com lacuna: a sequência viu', gaps == 1, gaps)
check('quadro sumido com lacuna: 14 faltando por canal', short == 14 * CH, short)

# One conversion that never reached anyone: one sample short, nothing else.
one = [c[:5000] + c[5001:] for c in chans]
code, short, extra, gaps, _ = judge(frames_of(one, 1000))
check('uma conversão a menos: FALHOU', code == 1)
check('uma conversão a menos: 1 por canal', short == CH, short)

# One conversion counted twice: one extra.
dup = [c[:5000] + [c[5000]] + c[5000:] for c in chans]
code, short, extra, gaps, _ = judge(frames_of(dup, 1000))
check('uma conversão duplicada: FALHOU', code == 1)
check('uma conversão duplicada: 1 sobrando por canal', extra == CH and short == 0, (short, extra))

print('%d verificações, %d falhas' % (checks, failures))
sys.exit(1 if failures else 0)
