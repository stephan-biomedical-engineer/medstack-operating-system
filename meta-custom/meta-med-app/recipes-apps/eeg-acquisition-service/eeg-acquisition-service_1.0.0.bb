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
    file://eeg-record-export.service \
    file://eeg-record-export.path \
"

S = "${WORKDIR}/sources"

DEPENDS = "med-framework-api"

inherit cmake pkgconfig systemd features_check

REQUIRED_DISTRO_FEATURES = "systemd"

# The exporter is in the same package on purpose: the EEG's device-specific
# delta is a number the thesis reports (RESULTS.md), and a clinical file is
# part of the same device application, not a new one.
SYSTEMD_SERVICE:${PN} = "eeg-acquisition.service eeg-record-export.service eeg-record-export.path"
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
    install -m 0644 ${WORKDIR}/eeg-record-export.service \
        ${D}${systemd_system_unitdir}/eeg-record-export.service
    install -m 0644 ${WORKDIR}/eeg-record-export.path \
        ${D}${systemd_system_unitdir}/eeg-record-export.path
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
    import re

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

    for prefix in ('device.option.', 'record.'):
        if any(key.startswith(prefix) for key in values):
            bb.fatal("eeg.conf already contains %s* lines. They are derived "
                     "from the afe.* prescription for MED_EEG_LINK and must not be "
                     "written by hand - two sources for one setting can disagree."
                     % prefix)

    def compact(x):
        return '%.10g' % x

    # A prescription the link cannot honour. Fatal for the one this device ships
    # with; for the alternatives an operator may choose between, it only means
    # that one is not offered.
    class Refusal(Exception):
        pass

    def refuse(message):
        raise Refusal(message)

    # One prescription in, what the link is told and what the record declares
    # out - or a Refusal. Written once and called for the shipped prescription
    # and for every alternative, so that a choice made on the device at run
    # time is translated by exactly the table that translates the factory one.
    def translate(values):
        def required(key):
            if key not in values:
                refuse("eeg.conf has no %s, and the front-end options for link '%s' "
                         "are derived from it" % (key, link))
            return values[key]

        def number(key):
            try:
                return float(required(key))
            except ValueError:
                refuse("eeg.conf: %s = '%s' is not a number" % (key, values[key]))

        def boolean(key):
            value = required(key).lower()
            if value in ('true', '1', 'yes', 'on'):
                return True
            if value in ('false', '0', 'no', 'off'):
                return False
            refuse("eeg.conf: %s = '%s' is not a boolean" % (key, values[key]))

        # The converter class this application was designed for resolves 24 bits on
        # every link: the simulated converter quantises to it, the co-processor's
        # firmware and the kernel driver read it. One number, so that the record's
        # declaration and the simulator's behaviour cannot drift apart.
        resolution_bits = 24

        gain = number('afe.gain')
        reference_uv = number('afe.reference_uv')
        lead_off = boolean('afe.lead_off_detection')
        bias = boolean('afe.bias_drive')
        test_signal = required('afe.test_signal')
        if test_signal not in ('off', 'internal'):
            refuse("eeg.conf: afe.test_signal must be 'off' or 'internal', not '%s'"
                     % test_signal)
        montage = required('afe.montage')
        if montage not in ('bipolar', 'referential'):
            refuse("eeg.conf: afe.montage must be 'bipolar' or 'referential', not '%s'"
                     % montage)
        if gain <= 0 or reference_uv <= 0:
            refuse("eeg.conf: afe.gain and afe.reference_uv must be positive")

        # The rates a converter of this class offers, each with the -3 dB band
        # of its sinc3 decimation filter at that rate (SBAS499C Tables 1-4, the
        # same at every gain - checked in implementation_plan_bdf.md phase 0).
        # A set and not a range, here as in the service's safety envelope.
        # Which of them a link delivers is that link's business, below.
        bandwidth = {250: 65, 500: 131, 1000: 262, 2000: 524, 4000: 1048,
                     8000: 2096, 16000: 4193}
        rate = number('acquisition.sample_rate_hz')
        if rate not in bandwidth:
            refuse("eeg.conf: acquisition.sample_rate_hz = %s is not a rate the "
                   "converter offers" % values['acquisition.sample_rate_hz'])

        # Two settings that take inputs off the electrodes, and they are not
        # alike.
        #
        # afe.input = shorted ties both inputs of EVERY channel together inside
        # the converter. What is recorded is the converter's own noise and
        # nothing of a patient: a bench measurement, and the record and the
        # clinical file say so.
        #
        # afe.temperature gives ONE channel - the last - to the converter's
        # temperature sensor and leaves the others on their electrodes. A
        # session goes on, one channel short, with the temperature recorded
        # beside it.
        #
        # The converter's switch has one more position, its supply rails. It
        # is not here: it reads 2.5 V, which only the lowest gain can
        # represent, and the "amp" link's frame cannot carry that gain's full
        # scale - so it could be offered on the simulation alone.
        source = required('afe.input')
        if source not in ('normal', 'shorted'):
            refuse("eeg.conf: afe.input must be 'normal' or 'shorted', not '%s'" % source)
        temperature = boolean('afe.temperature')
        try:
            channels = int(required('acquisition.channels'))
        except ValueError:
            refuse("eeg.conf: acquisition.channels = '%s' is not an integer"
                   % values['acquisition.channels'])
        # The test signal is a position of the same switch as the short.
        if source == 'shorted' and test_signal == 'internal':
            refuse("afe.input = shorted with afe.test_signal = internal asks the "
                   "input switch for two positions")
        # And shorted is every channel, the sensor's included.
        if source == 'shorted' and temperature:
            refuse("afe.input = shorted with afe.temperature = true: the last "
                   "channel cannot be both")

        if link == 'simulated':
            # The generator makes any rate; what carries it is the service, one
            # frame at a time, and a frame is 14 samples whatever the rate. The
            # record used to be the ceiling - every frame was synchronised
            # before the next was read, and on the QEMU image the suite asked
            # for 1000 samples a second and counted 858 (2026-10-13). It is now
            # written by the framework's record stream, off the acquisition
            # path, and on a host build every rate came out within 0.2 % up to
            # 8000 and within 0.8 % at 16000, where what runs short is this
            # generator's own pacing: its period is under a millisecond there,
            # and it resynchronises instead of bursting, so the shortfall is
            # not counted anywhere. The two rates at which it was seen are not
            # offered; the suite's configure-rate assertion measures 1000.
            if rate > 4000:
                refuse("acquisition.sample_rate_hz = %s is not kept on link 'simulated': "
                       "measured short of the declared rate"
                       % values['acquisition.sample_rate_hz'])
            # Lead-off and bias need no translation: a simulation has no electrodes,
            # so either value is trivially what it does.
            options = [
                ('unit', 'uV'),
                ('full_scale', compact(reference_uv / gain)),
                ('resolution_bits', str(resolution_bits)),
                ('noise_rms', '0.14'),
            ]
            if source == 'normal':
                # amplitude uV @ frequency Hz @ phase step per channel, rad
                options.append(('tones', '20@10@0.4, 5@50@0, 2@0.3@0.4'))
            # Shorted inputs: the noise floor above and nothing else.
            if temperature:
                # What the sensor reads at 27 degC, by the record's own
                # declaration below, on the last channel: nothing on the
                # simulation is warm, and the number only has to be one the
                # conversion turns into a plausible temperature. The tones
                # stay under it - the generator has no way to silence one
                # channel - and that is a property of the simulation.
                options.append(('offset', '%s@%d' % (compact(145300.0 + (27.0 - 25.0) * 490.0),
                                                      channels)))
            if test_signal == 'internal':
                options.append(('square', '%s@%s' % (compact(reference_uv / 2400.0),
                                                     compact(2048000.0 / 2 ** 21))))

        elif link == 'amp':
            # The firmware on the real-time core is this application's own producer,
            # so it speaks the prescription's words and answers each one in its
            # control acknowledgement.
            #
            # Two of the words are left unsaid when they say "as before":
            # afe.input = normal and afe.temperature = false. The control
            # message holds eight options and the prescription, with the rate,
            # is nine keys; the producer defines both absences as exactly those
            # values (and its host suite checks that it does), and the two can
            # never both be said, because shorted inputs exclude the sensor. So
            # eight always fit - and there is no room for a ninth: the next
            # setting this link gains needs a larger message, which is a new
            # version of the wire format.
            options = sorted((key, value) for key, value in values.items()
                             if key.startswith('afe.')
                             and not (key == 'afe.input' and source == 'normal')
                             and not (key == 'afe.temperature' and not temperature))
            # The rate, in the same words. The producer programs the converter
            # with it and states it in every frame header; the framework's
            # driver holds each header against the rate it was opened with.
            #
            # All seven are rows on this link, and NONE above 250 has been
            # measured on it (2026-10-13). That departs from the rule the
            # other links keep, on purpose: a rate cannot be measured on a
            # bench whose screen does not offer it. What stands in for the
            # measurement until then is that a loss here is not silent - a
            # conversion the real-time core cannot keep breaks the frame and
            # leaves a gap in the sequence, which the record's reader counts.
            # A rate the bench shows does not hold comes out of this table
            # with its number, like the two gains below.
            #
            # Measured since, on the board (2026-10-13), and all seven hold:
            # twelve-second sessions at each rate, the frames counted in the
            # record within 0.3 % of the declared rate, none missing from the
            # sequence, none failing its CRC, each converted to its clinical
            # file. They did not at first - 1000 delivered 436 - and what it
            # took is in the firmware: its data cache (it runs from DDR and
            # had none), its conversion in integers, and at 16000 a faster
            # read. implementation_plan_hmi_desktop.md, section 12.14.
            options.append(('acquisition.sample_rate_hz', compact(rate)))
            if len(options) > 8:
                refuse("the prescription for link 'amp' is %d options and its control "
                       "message holds 8" % len(options))
            # One combination the producer is known to refuse, said here so
            # that it is never offered: with the internal test signal there is
            # no electrode in the measurement, and the firmware will not
            # monitor the contact of one.
            if test_signal == 'internal' and lead_off:
                refuse("afe.test_signal = internal with afe.lead_off_detection = true "
                       "is refused by the producer on link 'amp'")
            # A gain the converter has and this link cannot carry: its frame
            # is 32-bit nanovolts, and the full scale of the two lowest gains
            # does not fit. The producer refuses them; saying so here keeps
            # them off the table an operator chooses from. Seen on the board,
            # 2026-10-12: "x2" offered on the screen and refused when picked.
            if reference_uv * 1000.0 / gain > 2147483647.0:
                refuse("afe.gain = %s has a full scale of %s uV, which the frame of "
                       "link 'amp' cannot carry" % (values['afe.gain'],
                                                    compact(reference_uv / gain)))
            # And its sibling: off the electrodes there is no contact to monitor.
            if source == 'shorted' and lead_off:
                refuse("afe.input = shorted with afe.lead_off_detection = true is "
                       "refused by the producer on link 'amp'")

        elif link in ('spi', 'usb'):
            # The kernel driver's sysfs attributes: hardwaregain is IIO standard ABI,
            # input_mux, test_signal, bias_drive, montage and lead_off are the
            # driver's own.
            #
            # The temperature sensor on the last channel is the kernel driver's
            # temperature_channel (implementation_plan_usb_parity.md, phase 4).
            # The driver runs that channel at the prescribed gain capped at 12,
            # takes it out of the bias derivation and of lead-off, and says the
            # gain in the channel's scale; the cap is the producer's on "amp"
            # too, where gain 24 read -79 C on the board (2026-10-12). Offered
            # before a bench has read it through this link, like the short
            # below, and for the same reason. The channel's step is declared
            # here, full_scale.<n>, from the same cap - and the framework holds
            # it against the kernel's scale for that channel, so a driver and a
            # table that disagree about the cap stop the start instead of
            # recording the sensor at twice its value.
            #
            # Shorted inputs are input_mux = shorted, every channel at once
            # (implementation_plan_usb_parity.md, phase 1). Offered before a
            # bench has read them through this link, and on purpose: the bench
            # that measures them needs the row on the screen, as with the rates
            # on "amp". What stands in until then is that the converter's
            # switch is programmed with the same bytes the producer on "amp"
            # writes (tests/ads1299 holds the driver against it), and that the
            # record says "shorted", so the clinical file never calls it EEG.
            #
            # Off the electrodes there is no contact to monitor, and the record
            # would say the electrodes were watched - the rule of "amp".
            if source == 'shorted' and lead_off:
                refuse("afe.input = shorted with afe.lead_off_detection = true: no "
                       "electrode is in the measurement on link '%s'" % link)
            # The rate reaches the kernel driver as sampling_frequency, written
            # by the framework from the device's sample rate, so there is no
            # option line for it. On "usb" 250 is both the converter's lowest
            # rate and the bridge's ceiling (BRINGUP_AFE.md, section 6): two HID
            # exchanges per conversion leave no faster one. On "spi" nothing
            # stands in the way and nothing has been measured.
            if rate != 250:
                if link == 'usb':
                    refuse("acquisition.sample_rate_hz = %s: the bridge of link 'usb' "
                           "delivers 250 samples per second and no more"
                           % values['acquisition.sample_rate_hz'])
                refuse("acquisition.sample_rate_hz = %s has not been measured on link "
                       "'%s'" % (values['acquisition.sample_rate_hz'], link))
            # Lead-off detection is the kernel driver's lead_off attribute since
            # 2026-10-14 (implementation_plan_usb_parity.md, phase 2): "dc" senses
            # every electrode - the reference electrode alone on the negative side
            # in referential - and "off" senses none. Before it the driver powered
            # the comparators at probe and enabled no channel, so "true" was
            # accepted here and honoured by nothing, and the record said the
            # electrodes were monitored. A kernel without the attribute makes the
            # driver refuse the option, as with the montage.
            #
            # The same rule as the producer on "amp": with the test signal on
            # every channel there is no electrode in the measurement.
            if test_signal == 'internal' and lead_off:
                refuse("afe.test_signal = internal with afe.lead_off_detection = true: "
                       "no electrode is in the measurement on link '%s'" % link)
            if gain != int(gain):
                refuse("eeg.conf: afe.gain = %s is not an integer" % values['afe.gain'])
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
                ('input_mux', 'test_signal' if internal else source),
                ('test_signal', '1x_slow' if internal else 'off'),
                ('bias_drive', 'derived' if bias else 'off'),
                # The same two words on both sides. A kernel without the attribute
                # makes the driver refuse the option and the service refuse to
                # start, which is the right outcome for a montage nobody applied.
                ('montage', montage),
                ('lead_off', 'dc' if lead_off else 'off'),
                # Always said, "none" included: the kernel keeps the setting
                # from one choice to the next, and a choice without it would
                # leave the sensor where the previous one put it.
                ('temperature_channel', str(channels - 1) if temperature else 'none'),
                # Not attributes, and never written to the device: the range and
                # the resolution, from which the framework takes the exact step.
                # The kernel publishes its scale as short text (five significant
                # digits for this converter), and a record scaled by that text is
                # off the converter's grid - the clinical file's converter refused
                # every session of this link for it, on the board, on 2026-10-10.
                # The kernel's text is still read, as the check on these two.
                ('full_scale', compact(reference_uv / gain)),
                ('resolution_bits', str(resolution_bits)),
            ]
            if temperature:
                options.append(('full_scale.%d' % (channels - 1),
                                compact(reference_uv / min(gain, 12))))

        else:
            refuse("MED_EEG_LINK = '%s' has no front-end option table; known links are "
                     "simulated, amp, spi and usb" % link)

        # What the record says about itself (implementation_plan_bdf.md, D7 and
        # D10). The clinical file is derived from the raw record by a separate
        # program, and that program must not reconstruct the scale from an eeg.conf
        # that may not be the one the session ran with - so the session's metadata
        # carries it, and it is derived here, once, by the same table that decides
        # what the link is told.
        #
        # The montage is what the link DOES, not what anyone wishes. By this point
        # afe.montage is that: "amp" forwards it and the producer answers for it
        # (a producer that cannot connect the reference refuses, and the service
        # does not start), "spi"/"usb" write it to the kernel driver's attribute
        # (a driver without one refuses, with the same outcome), and "simulated"
        # has no inputs, so either is what it does.

        # Where the operator put each electrode. Not part of the afe.* prescription
        # on purpose: do_derive forwards every afe.* key to the producer, and a
        # converter has no use for the name of a scalp position - nor would eight
        # of them fit in a control message's value.
        #
        # Optional. Without it the clinical file gets generic labels, which is a
        # valid file and an honest one; with it, every name is checked here,
        # because a label that does not fit the file's 16-character field would
        # otherwise be truncated into a different electrode's name.
        electrodes = values.get('montage.electrodes', '').split()
        reference = values.get('montage.reference', '')
        name = re.compile(r'^[A-Za-z0-9]+$')
        if electrodes:
            try:
                channels = int(required('acquisition.channels'))
            except ValueError:
                refuse("eeg.conf: acquisition.channels = '%s' is not an integer"
                         % values['acquisition.channels'])
            if len(electrodes) != channels:
                refuse("eeg.conf: montage.electrodes names %d electrodes for %d channels"
                         % (len(electrodes), channels))
            for electrode in electrodes:
                if montage == 'bipolar':
                    parts = electrode.split('-')
                    if len(parts) != 2 or not all(name.match(p) for p in parts):
                        refuse("eeg.conf: montage.electrodes entry '%s' is not a pair "
                                 "'A-B', and the montage is bipolar: each channel "
                                 "measures between two electrodes" % electrode)
                    label = 'EEG ' + electrode
                else:
                    if not name.match(electrode):
                        refuse("eeg.conf: montage.electrodes entry '%s' is not a "
                                 "single electrode name" % electrode)
                    label = 'EEG %s-%s' % (electrode, reference)
                if len(label) > 16:
                    refuse("eeg.conf: the label '%s' is %d characters, and the "
                             "clinical file's label field holds 16" % (label, len(label)))
        if montage == 'bipolar' and reference:
            refuse("eeg.conf: montage.reference is set, and the montage is bipolar: "
                     "there is no common reference to name")
        if montage == 'referential' and electrodes and not name.match(reference):
            refuse("eeg.conf: montage.reference = '%s' is not an electrode name"
                     % reference)

        record = [
            ('full_scale_uv', compact(reference_uv / gain)),
            ('resolution_bits', str(resolution_bits)),
            ('montage', montage),
            ('input', source),
        ]

        # The one channel that is not a voltage of interest, when there is
        # one: which it is, what to call it, and what its samples mean -
        #
        #     value = (microvolts - auxiliary_uv_at_reference) / auxiliary_uv_per_unit
        #             + auxiliary_reference
        #
        # Declared in every case, "none" included (channel 0), so that a
        # choice never inherits another choice's channel. The display and the
        # clinical file apply it as written; neither knows a sensor.
        #
        # The line is the converter's own [SBAS499C, "Temperature (degC) =
        # (reading - 145300 uV) / 490 uV/degC + 25"]. It is input-referred, so
        # it does not depend on the gain. Unverified until a bench session
        # warms the part and watches the number.
        if temperature:
            record += [('auxiliary_channel', str(channels)), ('auxiliary_label', 'Temp'),
                       ('auxiliary_unit', 'degC'), ('auxiliary_uv_per_unit', '490'),
                       ('auxiliary_uv_at_reference', '145300'), ('auxiliary_reference', '25')]
        else:
            record += [('auxiliary_channel', '0'), ('auxiliary_label', 'none'),
                       ('auxiliary_unit', 'uV'), ('auxiliary_uv_per_unit', '1'),
                       ('auxiliary_uv_at_reference', '0'), ('auxiliary_reference', '0')]

        # The converter's own filtering, for the clinical file's "prefiltering"
        # field: DC-coupled, and the band of its decimation filter at the rate
        # in force (the table above). Only the converter's: whatever
        # anti-aliasing the front-end board adds is not known and is not claimed.
        # The simulated link filters nothing and declares nothing.
        if link != 'simulated':
            record.append(('prefiltering', 'HP:DC LP:%dHz' % bandwidth[rate]))

        # The same cross-check the service makes before it will start: the
        # artefact threshold has to be a voltage this gain can represent. Made
        # here as well so that a combination the service would refuse is never
        # offered - and so that a factory prescription with this defect fails
        # the build instead of the boot.
        if 'safety.max_input_uv' in values and reference_uv / gain < number('safety.max_input_uv'):
            refuse("afe.gain = %s leaves a full scale of %s uV, below safety.max_input_uv = %s"
                   % (values['afe.gain'], compact(reference_uv / gain),
                      values['safety.max_input_uv']))

        return options, record

    try:
        options, record = translate(values)
    except Refusal as refusal:
        bb.fatal(str(refusal))

    # --- what an operator may choose instead -------------------------------
    # Every combination of the settings below that THIS link can honour, each
    # already translated by the table above, in a second sealed store. The
    # service changes a setting by picking a row and applying it as written: it
    # never translates, so its binary stays the same on every link, and a
    # display shows what this file lists and nothing else - which is how a link
    # that cannot do something comes to not offer it.
    #
    # The rate is one of them, and the rule for it is the rule for the rest: a
    # rate is a row for a link when that link has been measured delivering it.
    # Every link refuses the others, above, so what an operator sees on a link
    # with a converter is, for now, the one rate it is known to keep.
    import itertools

    domain = [
        ('afe.gain', ['1', '2', '4', '6', '8', '12', '24']),
        ('acquisition.sample_rate_hz', ['250', '500', '1000', '2000', '4000', '8000', '16000']),
        ('afe.lead_off_detection', ['true', 'false']),
        ('afe.montage', ['bipolar', 'referential']),
        ('afe.test_signal', ['off', 'internal']),
        ('afe.temperature', ['false', 'true']),
        ('afe.input', ['normal', 'shorted']),
    ]

    def normalised(key, value):
        if key in ('afe.gain', 'acquisition.sample_rate_hz'):
            return compact(float(value))
        if key in ('afe.lead_off_detection', 'afe.temperature'):
            return 'true' if value.lower() in ('true', '1', 'yes', 'on') else 'false'
        return value

    factory = tuple(normalised(key, values[key]) for key, _ in domain)
    rows = []
    refused = 0
    for combination in itertools.product(*(choices for _, choices in domain)):
        candidate = dict(values)
        for (key, _), value in zip(domain, combination):
            candidate[key] = value
        try:
            candidate_options, candidate_record = translate(candidate)
        except Refusal:
            refused += 1
            continue
        rows.append((combination, candidate_options, candidate_record))

    shipped = [index for index, row in enumerate(rows) if row[0] == factory]
    if len(shipped) != 1:
        bb.fatal("the shipped prescription %s is not one of the %d choices derived for "
                 "link '%s': the table an operator chooses from would not contain the "
                 "state the device starts in" % (dict(zip((k for k, _ in domain), factory)),
                                                 len(rows), link))

    # Nothing is written until everything has been derived. This task appends
    # to a file do_install produced; a run that failed half way used to leave
    # that file already appended to, and the next run then refused it as
    # hand-edited - a build that could only be repaired by cleaning the recipe.
    with open(config, 'a') as handle:
        handle.write("\n# --- device options, derived at build time for this link ------------------\n"
                     "# Generated by do_derive_device_options from the afe.* prescription above.\n"
                     "# Do not edit: change the prescription, and the recipe re-derives these.\n"
                     "# Link: %s\n" % link)
        for key, value in options:
            handle.write("device.option.%s = %s\n" % (key, value))
        handle.write("\n# --- what the record declares about itself, derived for this link ---------\n")
        for key, value in record:
            handle.write("record.%s = %s\n" % (key, value))

    bb.note("derived %d device options and %d record declarations for link %s"
            % (len(options), len(record), link))

    choices = os.path.join(os.path.dirname(config), 'eeg-choices.conf')
    with open(choices, 'w') as handle:
        handle.write("# MedPlatform EEG - what an operator may choose, on this link.\n"
                     "#\n"
                     "# Generated by do_derive_device_options and sealed like eeg.conf: the\n"
                     "# service refuses to load it without its sidecar. Each choice is one\n"
                     "# whole prescription and, under it, exactly what the link is told and\n"
                     "# what the record declares when it is in force. Do not edit.\n"
                     "# Link: %s\n\n" % link)
        handle.write("choices.count = %d\n" % len(rows))
        handle.write("choices.keys = %s\n" % ' '.join(key for key, _ in domain))
        handle.write("choices.factory = %d\n" % shipped[0])
        if link != 'simulated':
            # The front-end board's reference switch is manual and no software
            # can read it. Whoever changes the montage has to say where it is,
            # and the record carries that as said, not as known.
            handle.write("choices.affirm.afe.montage = reference_switch\n")
        for index, (combination, candidate_options, candidate_record) in enumerate(rows):
            handle.write("\n")
            for (key, _), value in zip(domain, combination):
                handle.write("choice.%d.%s = %s\n" % (index, key, value))
            for key, value in candidate_options:
                handle.write("choice.%d.device.option.%s = %s\n" % (index, key, value))
            for key, value in candidate_record:
                handle.write("choice.%d.record.%s = %s\n" % (index, key, value))

    bb.note("derived %d choices for link %s (%d combinations the link cannot honour)"
            % (len(rows), link, refused))
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

    # The table of choices is sealed by the same hand, for the same reason: a
    # device must not take a front-end setting from a file nobody vouched for.
    choices = os.path.join(os.path.dirname(config), 'eeg-choices.conf')
    if not os.path.exists(choices):
        bb.fatal("eeg-choices.conf was not derived; nothing to seal")

    for store in (config, choices):
        digest = bb.utils.sha256_file(store)
        sidecar = store + '.sha256'
        with open(sidecar, 'w') as handle:
            handle.write(digest + '\n')
        os.chmod(sidecar, 0o640)
        bb.note("sealed %s with sha256 %s" % (store, digest))
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
    ${systemd_system_unitdir}/eeg-record-export.service \
    ${systemd_system_unitdir}/eeg-record-export.path \
"
