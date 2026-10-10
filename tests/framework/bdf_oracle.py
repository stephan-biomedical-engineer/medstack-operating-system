#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The independent reader of the BDF+ files test_biosignal.cpp writes.

The C++ checks of BiosignalWriter were written from the same reading of the
specification as the writer, so they prove consistency, not correctness
(implementation_plan_bdf.md, phase 2). This script asks two readers nobody here
wrote - pyedflib, which wraps EDFlib, the library behind EDFbrowser, and MNE -
what the files say, and compares it with what the test wrote. The expected
codes are rebuilt below from the test's description of its ramp, not imported
from it.

    python3 -m venv V && V/bin/pip install pyedflib mne
    mkdir S && MED_BDF_SAMPLE_DIR=S build/med-framework-tests
    V/bin/python -I bdf_oracle.py S

Neither reader is a dependency of anything: they live in a throwaway venv on
the host, never in an image.
"""

import sys
import warnings

import mne
import pyedflib

LSB_UV = 375000.0 / 16777216.0   # 24 bits over +-4.5 V / 24, in microvolts
CHANNELS, PER_RECORD = 8, 14

failures = 0
checks = 0


def check(ok, what, detail=""):
    global failures, checks
    checks += 1
    if not ok:
        failures += 1
        print("   FALHA  %s %s" % (what, detail))


def ramp(base):
    """test_biosignal.cpp's ramp(): both signs, both 24-bit extremes."""
    out = []
    for i in range(CHANNELS * PER_RECORD):
        if i % 3 == 0:
            out.append(base + i)
        elif i % 3 == 1:
            out.append(-base - i)
        else:
            out.append(8388607 if i % 2 else -8388608)
    return out


def per_channel(records):
    """Interleaved records -> one list of codes per channel, in time order."""
    channels = [[] for _ in range(CHANNELS)]
    for record in records:
        for k in range(PER_RECORD):
            for c in range(CHANNELS):
                channels[c].append(record[k * CHANNELS + c])
    return channels


def continuous(path):
    print("-- %s (BDF+C)" % path)
    expected = per_channel([ramp(1000), ramp(-77), ramp(1000)])

    f = pyedflib.EdfReader(path)
    try:
        check(f.signals_in_file == CHANNELS, "pyedflib: sinais", f.signals_in_file)
        check(f.datarecords_in_file == 3, "pyedflib: registros", f.datarecords_in_file)
        check(abs(f.datarecord_duration - 0.056) < 1e-9, "pyedflib: duração", f.datarecord_duration)
        check(all(abs(x - 250.0) < 1e-9 for x in f.getSampleFrequencies()), "pyedflib: 250 Hz")
        check(f.getSignalLabels() == ["EEG %d+-%d-" % (c, c) for c in range(1, 9)],
              "pyedflib: rótulos", f.getSignalLabels())
        check(f.getPhysicalDimension(0) == "uV", "pyedflib: dimensão")
        check(f.getPrefilter(0) == "HP:DC LP:65Hz", "pyedflib: pré-filtragem")
        # The lossless claim, read by someone else: every code, exactly.
        for c in range(CHANNELS):
            got = [int(x) for x in f.readSignal(c, digital=True)]
            check(got == expected[c], "pyedflib: códigos do canal %d" % (c + 1))
        onsets, durations, texts = f.readAnnotations()
        got = sorted(zip([round(o, 6) for o in onsets], [round(d, 6) for d in durations], texts))
        # Readers count from the first sample, 0.356 s into the header second.
        check(got == [(0.0, -1.0, "montage bipolar"), (0.044, 0.056, "lead-off: 3+")],
              "pyedflib: anotações", got)
        check(f.getEquipment() == "MedPlatform eeg0 1.0.0", "pyedflib: equipamento",
              repr(f.getEquipment()))
    finally:
        f.close()

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        raw = mne.io.read_raw_bdf(path, preload=True, verbose="error")
    check(raw.info["sfreq"] == 250.0, "mne: 250 Hz", raw.info["sfreq"])
    check(raw.n_times == 3 * PER_RECORD, "mne: amostras", raw.n_times)
    data = raw.get_data() * 1e6
    worst = max(abs(data[c][k] - expected[c][k] * LSB_UV)
                for c in range(CHANNELS) for k in range(3 * PER_RECORD))
    # The 8-character physical fields round the scale: the true maximum,
    # 187499.9776 uV, can only be written "187500", which makes the step
    # 1/2^24 too large - one step by full scale, and no more. The codes
    # themselves are exact (checked through pyedflib above); this bounds what
    # a reader's microvolts can differ from the converter's.
    check(worst <= LSB_UV * (1 + 1e-9), "mne: valores em uV", "pior desvio %.6f uV" % worst)
    print("   mne: pior desvio do valor físico %.6f uV = %.3f LSB" % (worst, worst / LSB_UV))
    texts = sorted(a["description"] for a in raw.annotations)
    check(texts == ["lead-off: 3+", "montage bipolar"], "mne: anotações", texts)


def discontinuous(path):
    """What the readers do with BDF+D is recorded, not asserted to be right:
    implementation_plan_bdf.md phase 2 measured that neither shows the gap."""
    print("-- %s (BDF+D)" % path)
    try:
        pyedflib.EdfReader(path).close()
        print("   pyedflib: abriu")
    except OSError as error:
        print("   pyedflib: recusou - %s" % str(error).split(": ")[-1])

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        raw = mne.io.read_raw_bdf(path, preload=True, verbose="error")
    print("   mne: %d amostras (3 registros com uma lacuna de 2 entre o 1º e o 2º)" % raw.n_times)
    check(any(a["description"] == "gap: 2 frames" for a in raw.annotations),
          "mne: a anotação da lacuna chegou")


def main(argv):
    directory = argv[1] if len(argv) > 1 else "."
    continuous(directory + "/continuous.bdf")
    discontinuous(directory + "/discontinuous.bdf")
    print("\n%d verificações do oráculo, %d falhas" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
