#!/bin/sh
#
# MedOS: first-boot provisioning of the encrypted /data volume.
#
# wic cannot write a LUKS header, so med-partitions.wks creates med-data as a
# plain ext4 filesystem and this script converts it on first boot. It runs on
# every boot, provisions once, and opens the volume every time.
#
# @MED_DATA_KEY_SOURCE@ is substituted at build time from the KAS project file.

set -eu

DEV=/dev/disk/by-partlabel/med-data
MAPPER=med-data
KEY_SOURCE="@MED_DATA_KEY_SOURCE@"

# The pristine filesystem that med-partitions.wks writes. This pair is the
# whole basis for deciding that a device has never been provisioned - see "the
# safeguard" below.
PRISTINE_TYPE=ext4
PRISTINE_LABEL=med-data

# The volume key lives on the ESP, not on the rootfs.
#
# A development key written to /etc would be destroyed by the first RAUC
# update: the rootfs is an A/B slot and the update replaces it, so the device
# would come back up unable to decrypt its own patient records as the direct
# result of a *successful* update. The ESP is the only writable area of this
# layout that no update touches.
#
# It is mounted privately here rather than through /boot because
# local-fs.target is ordered after cryptsetup, so a unit that must run *before*
# cryptsetup cannot depend on boot.mount without creating a dependency cycle.
ESP=/dev/disk/by-partlabel/esp
ESP_MNT=/run/med-provision-esp
KEY_REL=medplatform/data.key

log() { echo "med-data-provision: $*"; }
fail() { echo "med-data-provision: $*" >&2; exit 1; }

is_mounted() { grep -q " $1 " /proc/mounts; }

cleanup() {
    if is_mounted "$ESP_MNT"; then
        umount "$ESP_MNT" || true
    fi
    rmdir "$ESP_MNT" 2>/dev/null || true
}
trap cleanup EXIT

mount_esp() {
    mkdir -p "$ESP_MNT"
    is_mounted "$ESP_MNT" && return 0
    mount -t vfat "$ESP" "$ESP_MNT" || fail "cannot mount the ESP ($ESP)"
}

# Opening is the same operation whether the volume was provisioned a second ago
# or three years ago, so both paths go through here.
open_volume() {
    [ -b "/dev/mapper/$MAPPER" ] && { log "already open"; return 0; }
    case "$KEY_SOURCE" in
    development)
        mount_esp
        [ -f "$ESP_MNT/$KEY_REL" ] || fail "volume key missing at ESP:/$KEY_REL - \
the volume is provisioned but its key is gone, and /data cannot be recovered \
without it"
        cryptsetup open --key-file "$ESP_MNT/$KEY_REL" "$DEV" "$MAPPER"
        umount "$ESP_MNT"
        ;;
    *)
        fail "cannot open the volume: key source '$KEY_SOURCE' has no open path"
        ;;
    esac
    log "opened /dev/mapper/$MAPPER"
}

[ -b "$DEV" ] || fail "$DEV does not exist"

# ---------------------------------------------------------------------------
# Already provisioned? Open it and stop. Idempotence is a requirement here, not
# a convenience: this unit runs on every boot.
# ---------------------------------------------------------------------------
if cryptsetup isLuks "$DEV"; then
    open_volume
    exit 0
fi

# ---------------------------------------------------------------------------
# The safeguard.
#
# "Has no LUKS header" is NOT sufficient evidence of a new device: a corrupted
# header produces exactly the same symptom, and reformatting there destroys
# patient records. The distinction needs a positive marker of "never
# provisioned", and the only trustworthy one available is the pristine
# filesystem wic wrote. luksFormat destroys that label by construction, so the
# pristine state cannot be reproduced by accident.
#
# Anything else - no label, a different label, an unknown filesystem, garbage -
# is a DAMAGED device: fail loudly, do not format. Requiring deliberate
# intervention is the correct outcome when the alternative is erasing medical
# records.
# ---------------------------------------------------------------------------
fstype=$(blkid -o value -s TYPE "$DEV" 2>/dev/null || true)
label=$(blkid -o value -s LABEL "$DEV" 2>/dev/null || true)

if [ "$fstype" != "$PRISTINE_TYPE" ] || [ "$label" != "$PRISTINE_LABEL" ]; then
    fail "refusing to provision: $DEV is not pristine (type='$fstype' \
label='$label', expected '$PRISTINE_TYPE'/'$PRISTINE_LABEL'). This is also what \
a damaged LUKS header looks like, so refusing to format over what may be \
patient data. This needs deliberate intervention."
fi

case "$KEY_SOURCE" in
development)
    log "pristine device; provisioning with a device-unique development key"
    ;;
tpm2)
    # Deliberately not implemented. There is no TPM on the simulation target,
    # so an implementation could not be exercised here - and unexercised
    # security metadata is precisely what this repository stopped shipping.
    # This lands with the STM32MP257 build; see
    # docs/implementation_plan_luks.md §5 step 5.
    fail "key source 'tpm2' is not implemented yet. Refusing to provision \
rather than silently falling back to a weaker key."
    ;;
*)
    fail "unknown MED_DATA_KEY_SOURCE '$KEY_SOURCE'"
    ;;
esac

mount_esp
mkdir -p "$(dirname "$ESP_MNT/$KEY_REL")"
KEYFILE="$ESP_MNT/$KEY_REL"

if [ ! -f "$KEYFILE" ]; then
    log "generating a device-unique volume key"
    # No status=none: dd here is busybox, which accepts only
    # if/of/bs/count/skip/seek and exits 1 on anything else.
    dd if=/dev/urandom of="$KEYFILE" bs=32 count=1 2>/dev/null
    # vfat carries no POSIX modes; this is best-effort and the limitation is
    # part of what makes 'development' a development key.
    chmod 0400 "$KEYFILE" 2>/dev/null || true
fi

log "formatting $DEV as LUKS2"
cryptsetup luksFormat --type luks2 --batch-mode --key-file "$KEYFILE" "$DEV"
cryptsetup open --key-file "$KEYFILE" "$DEV" "$MAPPER"
mkfs.ext4 -F -q -L "$MAPPER" "/dev/mapper/$MAPPER"
umount "$ESP_MNT"

log "provisioned; /data is now backed by a dm-crypt mapping"
