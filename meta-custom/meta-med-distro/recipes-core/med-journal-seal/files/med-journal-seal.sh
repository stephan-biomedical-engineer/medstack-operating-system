#!/bin/sh
# MedOS: provision the Forward Secure Sealing key of the journal.
#
# Seal=yes in 10-journald-audit.conf seals nothing on its own. journald seals
# only when a sealing key exists at /var/log/journal/<machine-id>/fss, and the
# key is created by "journalctl --setup-keys". Until 2026-11-02 nothing ran that
# command, so every journal this platform wrote was unsealed while the
# documentation said it was sealed (RESULTS.md section 5, correction of that
# date). journald gave no warning; "journalctl --verify" without a key checks
# structure only and kept printing PASS.
#
# Two keys come out of --setup-keys, and they have opposite custody rules:
#
#   * the SEALING key (fss) stays here. journald evolves it every interval and
#     discards the old state, so whoever takes the device later cannot re-seal
#     a period that is already sealed. That is the "forward secure" part.
#   * the VERIFICATION key must LEAVE the device. It can regenerate the sealing
#     key of any epoch, so a copy kept on the device - on the same disk, on the
#     key partition, anywhere root reaches - lets root rewrite the journal and
#     seal it again, and the seal proves nothing.
#
# The verification key is therefore never written to the journal or to any
# persistent file. It goes to a tmpfs file, root-only, for the provisioning
# boot alone, where the factory station collects it (make check does that on
# QEMU; on a board it is copied from the serial console). If nobody collects it,
# it is gone at the next reboot, and the seals of this identity cannot be
# verified by anyone: a provisioning that was not witnessed, which is the
# correct failure.
#
# One key per machine-id. An A/B update boots a slot with its own machine-id
# (BOOT_SLOT_AB_STM32MP2.md section 5), which gets its own journal directory
# and therefore its own key, provisioned on its first boot by this same unit.
set -eu

HANDOVER_DIR=/run/med-journal-seal
HANDOVER="${HANDOVER_DIR}/verify-key"
INTERVAL=15min

log() { echo "med-journal-seal: $*"; }
fail() { echo "med-journal-seal: $*" >&2; exit 1; }

MID=$(cat /etc/machine-id 2>/dev/null || true)
[ -n "$MID" ] || fail "no machine-id; refusing to provision a key for an unnamed identity"

DIR="/var/log/journal/${MID}"
[ -d "$DIR" ] || fail "${DIR} does not exist: the journal is not on persistent storage, so there is nothing to seal"

if [ -s "${DIR}/fss" ]; then
    # The unit's condition normally skips this case; reaching it means the key
    # appeared between the check and the start. Never overwrite a sealing key:
    # that would orphan every seal already written with it.
    log "sealing key already present for ${MID}, nothing to do"
    exit 0
fi

# stdout carries the verification key and nothing else when it is not a
# terminal, so it is captured here and never echoed: echoing it would write it
# into the very journal it protects.
KEY=$(journalctl --setup-keys --interval="${INTERVAL}" 2>/dev/null) \
    || fail "journalctl --setup-keys failed (is the journal directory writable?)"

case "$KEY" in
    */*-*) ;;
    *) fail "--setup-keys produced no verification key in the expected form" ;;
esac
[ -s "${DIR}/fss" ] || fail "--setup-keys returned but ${DIR}/fss does not exist"

umask 077
mkdir -p "$HANDOVER_DIR"
chmod 0700 "$HANDOVER_DIR"
printf '%s\n' "$KEY" > "$HANDOVER"
chmod 0400 "$HANDOVER"
KEY=

# journald loads the sealing key when it opens a journal file. The file it has
# open now was created without one, so it is rotated: the new active file is
# sealed from its first entry, and the old one is archived as the record of
# what happened before the device had a key.
journalctl --rotate

log "sealing key provisioned for machine-id ${MID} (interval ${INTERVAL})"
log "verification key handed over at ${HANDOVER} until the next reboot; it must leave the device"
