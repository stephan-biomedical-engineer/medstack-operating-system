#!/bin/sh
# MedOS: the persistent state that must outlive an A/B update.
#
# The audit trail (/var/log/journal) and the applications' state
# (/var/lib/medplatform: the audit chain's head, a therapy left unfinished)
# lived on the rootfs, which is inside an A/B slot: the next update rewrites
# the slot and takes them with it (AUD-06 of
# docs/implementation_plan_conformity.md; BOOT_SLOT_AB_STM32MP2.md section 5).
# /data is the one volume no update writes, and it is encrypted, which the
# trail - indirect clinical data - should be anyway (decision D8).
#
# This step runs after /data is mounted and before the two bind mounts. It
# creates their sources and, once, carries over what the rootfs already had,
# so a device that gains this unit by an update does not start its trail from
# nothing. It never overwrites: an existing source wins, because a source on
# /data is the trail and a copy on a freshly written slot is not.
set -eu

STATE=/data/state

log() { echo "med-state: $*"; }

grep -q " /data " /proc/mounts || { echo "med-state: /data is not mounted" >&2; exit 1; }
mkdir -p "$STATE"
chmod 0755 "$STATE"

carry() {
    # $1 source on /data, $2 the rootfs directory the bind mount will cover.
    if [ ! -d "$1" ]; then
        mkdir -p "$1"
        if [ -d "$2" ] && [ -n "$(ls -A "$2" 2>/dev/null)" ]; then
            cp -a "$2/." "$1/"
            log "carried $2 over to $1 (first boot with persistent state)"
        fi
    fi
}

carry "$STATE/journal" /var/log/journal
# journald's own layout: setgid systemd-journal, readable by that group.
chown root:systemd-journal "$STATE/journal"
chmod 2755 "$STATE/journal"

carry "$STATE/medplatform" /var/lib/medplatform
chown root:root "$STATE/medplatform"
chmod 0700 "$STATE/medplatform"
