#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Board side of the montage bench (ROTEIRO_PLACA.md §7.7): records three short
# sessions in a row - bipolar, referential, bipolar - and puts the sealed
# factory configuration back. Run from the PC, the board has no copy of it:
#
#     ssh $B sh -s < scripts/montage-bench.sh                  the three in a row
#     ssh $B sh -s -- referential < scripts/montage-bench.sh   one step: bipolar,
#                                                              referential or fabrica
#
# One step at a time is for a run in which the front-end board's switch is
# moved between sessions: move it, then run the step. Each step leaves the
# service acquiring in that configuration until the next one.
#
# The converter is the voltmeter. A multimeter on an electrode input loads the
# node it measures and reads something else every time; the converter does
# not. Lead-off detection is off in all three sessions, so that its current
# sources are not what moves the inputs, and the montage is the one variable.
#
# The front-end board's SRB1 switch is manual and no software can read it:
# whoever runs this decides its position for each session and writes it down
# beside the session name this prints. The record cannot say it.
#
# Nothing here survives a reboot (/run), and a reboot in the middle silently
# returns the board to the factory montage - do not reboot during the run.

set -e

SECONDS_EACH=40
BENCH=/run/med-bench
DROPIN=/run/systemd/system/eeg-acquisition.service.d

prepare() {  # $1 directory, $2 montage
    mkdir -p "$BENCH/$1"
    sed -e "s/^afe.lead_off_detection = true/afe.lead_off_detection = false/" \
        -e "s/^device.option.afe.lead_off_detection = true/device.option.afe.lead_off_detection = false/" \
        -e "s/^afe.montage = bipolar/afe.montage = $2/" \
        -e "s/^device.option.afe.montage = bipolar/device.option.afe.montage = $2/" \
        -e "s/^record.montage = bipolar/record.montage = $2/" \
        /etc/medplatform/eeg.conf > "$BENCH/$1/eeg.conf"
    (cd "$BENCH/$1" && sha256sum eeg.conf | cut -d" " -f1 > eeg.conf.sha256)
    # The sed must have matched: a copy that still says the factory values
    # would record the factory configuration under the test's name.
    test "$(grep -c "montage = $2\$" "$BENCH/$1/eeg.conf")" = 3
    test "$(grep -c "lead_off_detection = false\$" "$BENCH/$1/eeg.conf")" = 2
}

report() {
    systemctl show eeg-acquisition -p ActiveState -p NRestarts
    # Read now: the trace keeps about twelve lines and gains one every 10 s.
    tr -d '\0' < "$(ls /sys/kernel/debug/remoteproc/remoteproc*/trace0 | head -n 1)" |
        grep '\[afe\] configured' | tail -n 1
    ls -d /data/eeg/session-* | tail -n 1
}

record() {  # $1 directory
    echo "== $1"
    mkdir -p "$DROPIN"
    printf '[Service]\nExecStart=\nExecStart=/usr/bin/eeg-acquisition-service --config %s/%s/eeg.conf\n' \
        "$BENCH" "$1" > "$DROPIN/bench.conf"
    systemctl daemon-reload
    systemctl restart eeg-acquisition
    sleep 6
    report
    sleep "$SECONDS_EACH"
}

factory() {
    echo "== fabrica"
    rm -f "$DROPIN/bench.conf"
    systemctl daemon-reload
    systemctl restart eeg-acquisition
    sleep 6
    report
    systemctl show eeg-acquisition -p ExecStart | cut -c1-120
}

echo "uptime: $(cut -d' ' -f1 /proc/uptime) s"
case "${1:-}" in
    bipolar|referential)
        prepare "$1" "$1"
        record "$1"
        ;;
    fabrica)
        factory
        ;;
    "")
        prepare bipolar bipolar
        prepare referential referential
        record bipolar
        record referential
        record bipolar
        factory
        ;;
    *)
        echo "passo desconhecido: $1 (bipolar, referential, fabrica, ou nada)" >&2
        exit 2
        ;;
esac
echo "uptime: $(cut -d' ' -f1 /proc/uptime) s"
