#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host checks of the EEG service recipe's per-link translation
(implementation_plan_usb_parity.md, phase 1).

do_derive_device_options and do_seal_configuration are taken out of the recipe
as written - the text between `python <task>() {` and its closing brace - and
run with stand-ins for bitbake's `d` and `bb`, over the factory eeg.conf the
recipe installs. What comes out is the eeg.conf and the eeg-choices.conf an
image would ship, and the checks read those files, never the function's
internals. A build would find a refusal too, but only by building; this finds
it in a second, and finds what a build cannot: a table that is consistent and
wrong.
"""

import hashlib
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RECIPE_DIR = os.path.join(HERE, '..', '..', 'meta-custom', 'meta-med-app', 'recipes-apps',
                          'eeg-acquisition-service')
RECIPE = os.path.join(RECIPE_DIR, 'eeg-acquisition-service_1.0.0.bb')
FACTORY = os.path.join(RECIPE_DIR, 'files', 'eeg.conf')
DRIVER = {'simulated': 'simulated', 'amp': 'rpmsg', 'spi': 'iio', 'usb': 'iio'}
checks = failures = 0


def check(what, ok, got=None):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print('FALHA: %s%s' % (what, '' if got is None else ' (obtido: %r)' % (got,)))


class Fatal(Exception):
    pass


class _Utils:
    @staticmethod
    def sha256_file(path):
        with open(path, 'rb') as handle:
            return hashlib.sha256(handle.read()).hexdigest()


class _BB:
    utils = _Utils

    @staticmethod
    def fatal(message):
        raise Fatal(message)

    @staticmethod
    def note(message):
        pass


def task(text, name):
    found = re.search(r'^python %s\(\) \{\n(.*?)^\}\n' % name, text, re.S | re.M)
    if not found:
        raise SystemExit('a tarefa %s não está na receita' % name)
    namespace = {}
    exec('def %s(d, bb):\n%s' % (name, found.group(1)), namespace)
    return namespace[name]


with open(RECIPE) as handle:
    _text = handle.read()
derive = task(_text, 'do_derive_device_options')
seal = task(_text, 'do_seal_configuration')


def parse(path):
    values = {}
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                key, value = line.split('=', 1)
                values[key.strip()] = value.strip()
    return values


def run(link, **prescription):
    """Derive and seal for a link, with factory keys overridden by
    prescription (dots as double underscores). Returns (eeg.conf values,
    the choices as a list of dicts, the fatal message or None)."""
    with open(FACTORY) as handle:
        conf = handle.read()
    for placeholder, value in (('@MED_EEG_DRIVER@', DRIVER[link]), ('@MED_EEG_ADDRESS@', ''),
                               ('@MED_EEG_LINK@', link), ('@MED_EEG_REQUIRE_ENCRYPTION@', 'false')):
        conf = conf.replace(placeholder, value)
    for key, value in prescription.items():
        key = key.replace('__', '.')
        conf, n = re.subn(r'^%s = .*$' % re.escape(key), '%s = %s' % (key, value), conf,
                          flags=re.M)
        if n != 1:
            raise SystemExit('a chave %s não está no eeg.conf de fábrica' % key)

    with tempfile.TemporaryDirectory() as root:
        etc = os.path.join(root, 'etc', 'medplatform')
        os.makedirs(etc)
        with open(os.path.join(etc, 'eeg.conf'), 'w') as handle:
            handle.write(conf)

        class D:
            @staticmethod
            def getVar(name):
                return {'D': root, 'sysconfdir': '/etc', 'MED_EEG_LINK': link,
                        'MED_EEG_DRIVER': DRIVER[link]}[name]

        try:
            derive(D, _BB)
            seal(D, _BB)
        except Fatal as fatal:
            return None, None, str(fatal)

        for store in ('eeg.conf', 'eeg-choices.conf'):
            path = os.path.join(etc, store)
            with open(path + '.sha256') as handle:
                check('%s: %s selado com o próprio sha256' % (link, store),
                      handle.read().strip() == _Utils.sha256_file(path))
        values = parse(os.path.join(etc, 'eeg.conf'))
        table = parse(os.path.join(etc, 'eeg-choices.conf'))

    rows = []
    for index in range(int(table['choices.count'])):
        prefix = 'choice.%d.' % index
        rows.append({k[len(prefix):]: v for k, v in table.items() if k.startswith(prefix)})
    return values, rows, None


# --- every link derives, and its table has the size it had ------------------
# A change of size is not a failure of the translation; it is a change of what
# an operator is offered, and it has to be said - here, with the reason.
expected = {
    'simulated': 700,
    'amp': 490,
    # 42 until phase 1 (2026-10-15); +14 = shorted inputs, 7 gains x 2 montages,
    # lead-off off, no test signal, no sensor.
    'spi': 56,
    'usb': 56,
}
tables = {}
for link, count in expected.items():
    values, rows, fatal = run(link)
    check('%s: a prescrição de fábrica deriva' % link, fatal is None, fatal)
    if fatal:
        continue
    tables[link] = rows
    check('%s: %d linhas na tabela' % (link, count), len(rows) == count, len(rows))

# --- on no link with a converter is a contact monitored with no electrode in
# the measurement. "simulated" is left out of that one on purpose: it has no
# electrodes at all, so either value is what it does, and the recipe says so.
for link, rows in tables.items():
    for row in rows:
        shorted = row['afe.input'] == 'shorted'
        if link != 'simulated' and shorted and row['afe.lead_off_detection'] == 'true':
            check('%s: curto com lead-off ligado não é linha' % link, False, row)
        if shorted and row['afe.test_signal'] == 'internal':
            check('%s: curto com sinal de teste não é linha' % link, False, row)
        if shorted and row['afe.temperature'] == 'true':
            check('%s: curto com sensor não é linha' % link, False, row)
        check('%s: o registro declara a entrada da linha' % link,
              row['record.input'] == row['afe.input'], row['record.input'])

# --- the shorted rows of the kernel-driven links ----------------------------
for link in ('spi', 'usb'):
    rows = tables.get(link, [])
    shorted = [row for row in rows if row['afe.input'] == 'shorted']
    check('%s: 14 linhas de curto' % link, len(shorted) == 14, len(shorted))
    check('%s: o curto chega ao kernel como input_mux = shorted' % link,
          all(row['device.option.input_mux'] == 'shorted' for row in shorted))
    check('%s: o curto chega com lead_off = off' % link,
          all(row['device.option.lead_off'] == 'off' for row in shorted))
    check('%s: o curto chega com o gerador desligado' % link,
          all(row['device.option.test_signal'] == 'off' for row in shorted))
    check('%s: as duas montagens e os sete ganhos' % link,
          {(row['afe.montage'], row['afe.gain']) for row in shorted}
          == {(m, g) for m in ('bipolar', 'referential')
              for g in ('1', '2', '4', '6', '8', '12', '24')})
    # And the other way: nothing that is not a short reaches the switch as one.
    normal = [row for row in rows if row['afe.input'] == 'normal']
    check('%s: entrada normal nunca chega como curto' % link,
          all(row['device.option.input_mux']
              == ('test_signal' if row['afe.test_signal'] == 'internal' else 'normal')
              for row in normal))

# --- a factory prescription the link cannot honour fails the build ----------
for link in ('spi', 'usb', 'amp'):
    _, _, fatal = run(link, afe__input='shorted')   # the factory has lead-off on
    check('%s: curto de fábrica com lead-off ligado recusa o build' % link,
          fatal is not None and 'afe.input = shorted' in fatal, fatal)
    values, _, fatal = run(link, afe__input='shorted', afe__lead_off_detection='false')
    check('%s: curto de fábrica com lead-off desligado deriva' % link, fatal is None, fatal)
    if values:
        check('%s: o registro de fábrica diz shorted' % link,
              values.get('record.input') == 'shorted', values.get('record.input'))
        if link != 'amp':
            check('%s: o eeg.conf de fábrica diz input_mux = shorted' % link,
                  values.get('device.option.input_mux') == 'shorted')

_, _, fatal = run('usb', afe__temperature='true')
check('usb: sensor de fábrica ainda recusa o build (fase 4)',
      fatal is not None and 'afe.temperature' in fatal, fatal)

print('%d verificações, %d falhas' % (checks, failures))
sys.exit(1 if failures else 0)
