#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Board side of the AMP bench (implementation_plan_m33_firmware.md, phase 3).
# Busybox only; the host side, which writes the control message and judges
# the capture, is scripts/amp-bench.py.
#
#   sh amp-bench.sh FIRMWARE CONTROL FRAMES OUT
#
# Loads FIRMWARE (a signed image) on the M33, opens the rpmsg-raw channel it
# announces, sends CONTROL, records the answer and FRAMES frames to OUT, and
# prints the Linux-side elapsed time and the firmware's trace buffer.
#
# Read and write go through ONE descriptor: rpmsg_char refuses a second open
# of the same endpoint with -EBUSY. And the firmware is STOPPED at the end:
# on an rpmsg-raw channel the kernel keeps queueing messages after the reader
# closes (rpmsg_ept_cb has no notion of "nobody is reading"), so a firmware
# left streaming grows a kernel queue without bound.

set -u
FW=$1 CTL=$2 FRAMES=$3 OUT=$4

uptime_cs() { awk '{ printf "%d\n", $1 * 100 }' /proc/uptime; }

R=""
for r in /sys/class/remoteproc/remoteproc*; do
	[ "$(cat "$r/name")" = m33 ] && R=$r
done
[ -n "$R" ] || { echo "no m33 remoteproc"; exit 1; }

[ "$(cat "$R/state")" = running ] && echo stop > "$R/state"
echo -n "$(dirname "$FW")" > /sys/module/firmware_class/parameters/path
basename "$FW" > "$R/firmware"
dmesg -c > /dev/null
echo start > "$R/state" || { echo "start failed"; dmesg | tail -5; exit 1; }

DEV=""
i=0
while [ -z "$DEV" ] && [ $i -lt 50 ]; do
	for n in /sys/class/rpmsg/*/name; do
		[ -e "$n" ] || continue
		d=$(dirname "$n")
		case "$(basename "$d")" in rpmsg_ctrl*) continue ;; esac
		[ "$(cat "$n")" = rpmsg-raw ] && [ -e "/dev/$(basename "$d")" ] && DEV=/dev/$(basename "$d")
	done
	[ -z "$DEV" ] && { sleep 0.1; i=$((i + 1)); }
done
echo "state: $(cat "$R/state"); channel: ${DEV:-NONE} after $((i * 100)) ms"
dmesg | grep -E 'rpmsg|remoteproc' | sed 's/^/  /'
[ -n "$DEV" ] || { echo stop > "$R/state"; exit 1; }

exec 3<>"$DEV"
T0=$(uptime_cs)
cat "$CTL" >&3
dd bs=512 count=$((FRAMES + 1)) <&3 > "$OUT" 2> /dev/null
T1=$(uptime_cs)
exec 3>&-
echo "captured $(wc -c < "$OUT") bytes in $(( (T1 - T0) / 100 )).$(printf %02d $(( (T1 - T0) % 100 ))) s (Linux clock)"

# The trace BEFORE the stop: trace0 is removed from debugfs with the
# firmware, so reading it afterwards prints nothing at all.
echo "--- trace0"
cat /sys/kernel/debug/remoteproc/$(basename "$R")/trace0 2> /dev/null | tr -d '\0' | tail -n 40
echo stop > "$R/state"
echo "state after: $(cat "$R/state")"
