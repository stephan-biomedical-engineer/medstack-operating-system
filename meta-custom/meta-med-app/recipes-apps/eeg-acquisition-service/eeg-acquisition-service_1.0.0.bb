SUMMARY = "MedPlatform EEG acquisition service (MedApp reference application)"
DESCRIPTION = "Data daemon of the EEG proof of concept. Reads sample frames \
from the acquisition front-end, persists them to the encrypted record volume, \
publishes them to the HMI and keeps the audit trail - all through MedFramework \
APIs, with no direct use of the OS or the BSP. Retargeting it to different \
hardware is a configuration change (MED_EEG_DRIVER), not a code change."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Source tree in a subdirectory, not directly in WORKDIR - see the same note in
# med-framework-api_1.0.0.bb: S = ${WORKDIR} puts PKGD inside S and the
# debug-source hardlinks of do_package make pseudo abort on any task re-run.
# eeg.conf and the unit file stay in WORKDIR because do_install reads them from
# there, and they are not part of the compiled tree.
SRC_URI = " \
    file://CMakeLists.txt;subdir=sources \
    file://src;subdir=sources \
    file://eeg.conf \
    file://eeg-acquisition.service \
"

S = "${WORKDIR}/sources"

DEPENDS = "med-framework-api"

inherit cmake pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd"

SYSTEMD_SERVICE:${PN} = "eeg-acquisition.service"
SYSTEMD_AUTO_ENABLE = "enable"

# The knobs that make the same application binary serve every target and every
# physical link. Set from the KAS project file:
#   project-eeg-qemu.yml      -> simulated front-end, no converter at all
#   project-eeg-stm32mp2.yml  -> rpmsg front-end on the Cortex-M33
#
# MED_EEG_LINK is not a driver name and is not redundant with MED_EEG_DRIVER:
# the "iio" driver serves two physically different links - a converter on the
# host's SPI bus, and one behind a USB bridge - whose timestamps mean different
# things. The record has to be able to say which, so the value is substituted
# into eeg.conf and travels into the session metadata.
#
# The defaults are the simulation profile's, which is the safe direction: a
# profile that forgets to declare its link gets "simulated", and a record that
# says "simulated" cannot be mistaken for a measurement.
MED_EEG_DRIVER ?= "simulated"
MED_EEG_ADDRESS ?= ""
MED_EEG_LINK ?= "simulated"
MED_EEG_REQUIRE_ENCRYPTION ?= "true"

do_install:append() {
    install -d ${D}${sysconfdir}/medplatform
    sed -e "s|@MED_EEG_DRIVER@|${MED_EEG_DRIVER}|g" \
        -e "s|@MED_EEG_ADDRESS@|${MED_EEG_ADDRESS}|g" \
        -e "s|@MED_EEG_LINK@|${MED_EEG_LINK}|g" \
        -e "s|@MED_EEG_REQUIRE_ENCRYPTION@|${MED_EEG_REQUIRE_ENCRYPTION}|g" \
        ${WORKDIR}/eeg.conf > ${D}${sysconfdir}/medplatform/eeg.conf
    chmod 0640 ${D}${sysconfdir}/medplatform/eeg.conf

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/eeg-acquisition.service \
        ${D}${systemd_system_unitdir}/eeg-acquisition.service
}

# The afe.* block of eeg.conf is this device's prescription, in its own words,
# and MedFramework deliberately has no vocabulary for it: the framework knows
# transports, and a parser there that knew "gain" or "lead-off" is exactly how
# it once became an EEG framework. So the translation into what the producer on
# the active link understands happens here, in the application layer, at build
# time - one table per link, appended to eeg.conf as device.option.* lines and
# sealed with the rest of the file.
#
# Build time rather than run time for two reasons. The service binary stays
# free of any conditional on the link, which is the portability claim. And a
# prescription a link cannot honour fails here, in a second, naming the key,
# instead of at boot.
#
# The numbers in the "simulated" table describe the converter class this
# application was designed for - a 24-bit converter with a noise floor of
# about 0.14 uV rms and a test generator swinging VREF/2400 at f_CLK/2^21 -
# plus a synthetic EEG made of an alpha rhythm, mains interference and drift.
# They were in the framework before, which is where they must not be; they are
# the application's facts about its own front-end, like the gain set in its
# safety table.
python do_derive_device_options() {
    import os

    config = os.path.join(d.getVar('D') + d.getVar('sysconfdir'), 'medplatform', 'eeg.conf')
    link = d.getVar('MED_EEG_LINK')
    driver = d.getVar('MED_EEG_DRIVER')

    # The table is chosen by the link, and the vocabulary belongs to the driver
    # that will read it. A profile whose two variables disagree would get
    # options for one driver delivered to another, which refuses all of them -
    # at boot. Here it costs a second.
    expected_driver = {'simulated': 'simulated', 'amp': 'rpmsg', 'spi': 'iio', 'usb': 'iio'}
    if link in expected_driver and driver != expected_driver[link]:
        bb.fatal("MED_EEG_LINK = '%s' is driven by MED_EEG_DRIVER = '%s', not '%s'"
                 % (link, expected_driver[link], driver))

    values = {}
    with open(config) as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped or stripped.startswith('#') or '=' not in stripped:
                continue
            key, value = stripped.split('=', 1)
            values[key.strip()] = value.strip()

    if any(key.startswith('device.option.') for key in values):
        bb.fatal("eeg.conf already contains device.option.* lines. They are derived "
                 "from the afe.* prescription for MED_EEG_LINK and must not be "
                 "written by hand - two sources for one setting can disagree.")

    def required(key):
        if key not in values:
            bb.fatal("eeg.conf has no %s, and the front-end options for link '%s' "
                     "are derived from it" % (key, link))
        return values[key]

    def number(key):
        try:
            return float(required(key))
        except ValueError:
            bb.fatal("eeg.conf: %s = '%s' is not a number" % (key, values[key]))

    def boolean(key):
        value = required(key).lower()
        if value in ('true', '1', 'yes', 'on'):
            return True
        if value in ('false', '0', 'no', 'off'):
            return False
        bb.fatal("eeg.conf: %s = '%s' is not a boolean" % (key, values[key]))

    def compact(x):
        return '%.10g' % x

    gain = number('afe.gain')
    reference_uv = number('afe.reference_uv')
    lead_off = boolean('afe.lead_off_detection')
    bias = boolean('afe.bias_drive')
    test_signal = required('afe.test_signal')
    if test_signal not in ('off', 'internal'):
        bb.fatal("eeg.conf: afe.test_signal must be 'off' or 'internal', not '%s'"
                 % test_signal)
    if gain <= 0 or reference_uv <= 0:
        bb.fatal("eeg.conf: afe.gain and afe.reference_uv must be positive")

    if link == 'simulated':
        # Lead-off and bias need no translation: a simulation has no electrodes,
        # so either value is trivially what it does.
        options = [
            ('unit', 'uV'),
            ('full_scale', compact(reference_uv / gain)),
            ('resolution_bits', '24'),
            ('noise_rms', '0.14'),
            # amplitude uV @ frequency Hz @ phase step per channel, rad
            ('tones', '20@10@0.4, 5@50@0, 2@0.3@0.4'),
        ]
        if test_signal == 'internal':
            options.append(('square', '%s@%s' % (compact(reference_uv / 2400.0),
                                                 compact(2048000.0 / 2 ** 21))))

    elif link == 'amp':
        # The firmware on the real-time core is this application's own producer,
        # so it speaks the prescription's words and answers each one in its
        # control acknowledgement.
        options = sorted((key, value) for key, value in values.items()
                         if key.startswith('afe.'))

    elif link in ('spi', 'usb'):
        # The kernel driver's sysfs attributes: hardwaregain is IIO standard ABI,
        # input_mux and test_signal are the driver's own. Two prescriptions have
        # no attribute at all, and are checked against what the driver does at
        # probe instead of being passed and ignored.
        if not lead_off:
            bb.fatal("afe.lead_off_detection = false cannot be honoured on link '%s': "
                     "the kernel driver enables the lead-off comparators at probe and "
                     "offers no control to switch them off" % link)
        if gain != int(gain):
            bb.fatal("eeg.conf: afe.gain = %s is not an integer" % values['afe.gain'])
        internal = test_signal == 'internal'
        # The prescription is a boolean and the part offers three
        # configurations, so the translation has to choose - and the choice is
        # clinical, which is why it is made here and written down rather than
        # defaulted inside the driver.
        #
        # "on" means DERIVED: the amplifier drives the inverse of the average of
        # every enabled channel, which is the driven-electrode arrangement that
        # rejects common mode. The other one the driver offers, "reference",
        # drives a mid-supply with no feedback from anything measured - a DC
        # bias and no rejection. A prescription asking for bias drive is asking
        # for the first; if a montage ever wants the second, it needs a
        # prescription that can say so, not a different default here.
        options = [
            ('hardwaregain', str(int(gain))),
            ('input_mux', 'test_signal' if internal else 'normal'),
            ('test_signal', '1x_slow' if internal else 'off'),
            ('bias_drive', 'derived' if bias else 'off'),
        ]

    else:
        bb.fatal("MED_EEG_LINK = '%s' has no front-end option table; known links are "
                 "simulated, amp, spi and usb" % link)

    with open(config, 'a') as handle:
        handle.write("\n# --- device options, derived at build time for this link ------------------\n"
                     "# Generated by do_derive_device_options from the afe.* prescription above.\n"
                     "# Do not edit: change the prescription, and the recipe re-derives these.\n"
                     "# Link: %s\n" % link)
        for key, value in options:
            handle.write("device.option.%s = %s\n" % (key, value))

    bb.note("derived %d device options for link %s" % (len(options), link))
}
addtask derive_device_options after do_install before do_seal_configuration
do_derive_device_options[fakeroot] = "1"
do_derive_device_options[depends] += "virtual/fakeroot-native:do_populate_sysroot"
do_derive_device_options[vardeps] += "MED_EEG_LINK"

# MedicalConfiguration refuses to load a store it cannot verify, so the
# integrity sidecar has to be produced by whoever produced the store - here,
# the build. Doing it in Python rather than shelling out to sha256sum keeps the
# task independent of what happens to be in HOSTTOOLS.
python do_seal_configuration() {
    import os
    import re

    config = os.path.join(d.getVar('D') + d.getVar('sysconfdir'), 'medplatform', 'eeg.conf')
    if not os.path.exists(config):
        bb.fatal("eeg.conf was not installed; nothing to seal")

    # Two checks on the substituted file, before it is sealed and therefore
    # before it can reach a device. Both of them have already caught a real
    # defect in this recipe, which is the standard this repository holds an
    # assertion to: one that has never seen the failure it looks for is a claim.
    #
    # They live here rather than in the acceptance suite because of where they
    # cost the least. The suite finds these too - it greps the shipped file for
    # leftover placeholders, and the service refuses to start on an unparsable
    # calibration value - but only after a full image build and a QEMU boot.
    # Here it is a second, and the message names the line.
    with open(config) as handle:
        lines = handle.readlines()

    leftover = [(n, l.rstrip()) for n, l in enumerate(lines, 1)
                if re.search(r'@[A-Z][A-Z0-9_]*@', l)]
    if leftover:
        bb.fatal("eeg.conf still contains build-time placeholders after "
                 "substitution:\n  " + "\n  ".join(
                     "line %d: %s" % (n, l) for n, l in leftover) +
                 "\nEither the recipe is missing a sed rule, or a comment "
                 "spells a placeholder out literally - which is the same thing "
                 "to anyone reading the shipped file.")

    # A value runs to the end of its line: MedicalConfiguration::parse takes
    # everything after the first "=" and trims whitespace only. An inline "#"
    # therefore becomes part of the value, "afe.gain = 24  # PGA" is not a
    # number, and the safety envelope stops the service at boot. Refusing to
    # parse is correct - a calibration value the platform cannot read must never
    # be guessed at - so the fix belongs here, at the point where the file is
    # written.
    inline = [(n, l.rstrip()) for n, l in enumerate(lines, 1)
              if not l.lstrip().startswith('#') and '=' in l
              and '#' in l.split('=', 1)[1]]
    if inline:
        bb.fatal("eeg.conf has an inline comment in a value, which becomes part "
                 "of the value:\n  " + "\n  ".join(
                     "line %d: %s" % (n, l) for n, l in inline) +
                 "\nPut the comment on its own line.")

    digest = bb.utils.sha256_file(config)
    sidecar = config + '.sha256'
    with open(sidecar, 'w') as handle:
        handle.write(digest + '\n')
    os.chmod(sidecar, 0o640)
    bb.note("sealed %s with sha256 %s" % (config, digest))
}
addtask seal_configuration after do_derive_device_options before do_package

# The task writes into ${D}, so it has to run under pseudo like do_install
# does. base.bbclass grants that to do_install only; a task added with addtask
# inherits nothing, and without these two flags the sidecar is created with the
# build user's uid instead of root:root. QA catches it as
# [host-user-contaminated], and it would ship a device whose configuration seal
# is owned by an unprivileged account - the one file that must not be.
do_seal_configuration[fakeroot] = "1"
do_seal_configuration[depends] += "virtual/fakeroot-native:do_populate_sysroot"

FILES:${PN} += " \
    ${sysconfdir}/medplatform \
    ${systemd_system_unitdir}/eeg-acquisition.service \
"
