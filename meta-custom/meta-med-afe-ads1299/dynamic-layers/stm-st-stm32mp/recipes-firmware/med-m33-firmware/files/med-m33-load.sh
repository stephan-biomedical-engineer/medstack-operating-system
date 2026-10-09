#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Load (or stop) the MedPlatform producer on the Cortex-M33.
#
#   med-m33-load start | stop
#
# Run by med-m33-firmware.service, which udev starts when the remoteproc called
# "m33" appears (90-med-amp.rules). The core is found by its NAME and never by
# its index: remoteproc0 and remoteproc1 swapped roles between two boots of the
# same board, and a fixed index sent this firmware to the M0+ once
# (BRINGUP_STM32MP2.md §9.15).
#
# Every failure is an exit status, so the unit lands in "failed" where
# `systemctl --failed` shows it - a co-processor that did not start is a device
# that cannot acquire, and that must not be discovered by the acquisition
# service timing out on a channel nobody announced.

set -u

FW=@MED_M33_FW@_sign.bin
R=""
for r in /sys/class/remoteproc/remoteproc*; do
	[ -e "$r/name" ] && [ "$(cat "$r/name")" = m33 ] && R=$r
done
[ -n "$R" ] || { echo "med-m33-load: no remoteproc called m33" >&2; exit 1; }

state() { cat "$R/state"; }

case "${1:-}" in
start)
	# A firmware already running here was started by someone else - the bench,
	# or a previous start of this unit - and may be streaming to no reader.
	# Start from a clean core rather than adopt it.
	[ "$(state)" = running ] && echo stop > "$R/state"
	echo "$FW" > "$R/firmware" || exit 1
	echo start > "$R/state" || { echo "med-m33-load: $FW refused" >&2; exit 1; }
	[ "$(state)" = running ] || { echo "med-m33-load: state $(state)" >&2; exit 1; }
	echo "med-m33-load: $FW running on $(basename "$R")"
	;;
stop)
	# The firmware powers the converter down before it acknowledges the stop
	# (CoproSync_ShutdownCb), so this is also what leaves the front-end safe.
	[ "$(state)" = running ] && echo stop > "$R/state"
	exit 0
	;;
*)
	echo "usage: $0 start|stop" >&2
	exit 2
	;;
esac
