#!/bin/sh
#
# MedOS: first-boot provisioning of the encrypted /data volume.
#
# wic cannot write a LUKS header, so every med-partitions layout creates
# med-data as a
# plain ext4 filesystem and this script converts it on first boot. It runs on
# every boot, provisions once, and opens the volume every time.
#
# @MED_DATA_KEY_SOURCE@ is substituted at build time from the KAS project file;
# @MED_KEY_STORE_DEV@ and @MED_KEY_STORE_FSTYPE@ come from meta-med-bsp, keyed
# on machine. This file names no partition and no filesystem of its own - see
# the note above the key store below for why that matters.

set -eu

DEV=/dev/disk/by-partlabel/med-data
MAPPER=med-data
KEY_SOURCE="@MED_DATA_KEY_SOURCE@"

# The pristine filesystem the med-partitions layouts write. This pair is the
# whole basis for deciding that a device has never been provisioned - see "the
# safeguard" below.
PRISTINE_TYPE=ext4
PRISTINE_LABEL=med-data

# Where the development key lives. NOT on the rootfs, and that is a requirement
# rather than a preference: the rootfs is an A/B slot, so a key written to /etc
# would be destroyed by the first *successful* RAUC update, leaving the device
# unable to decrypt its own patient records as the direct result of an update
# working. The key store must be a partition that no update touches.
#
# Which partition that is depends on the disk, and the disk depends on the
# board: the EFI layout has an ESP (vfat), the STM32MP2 layout has a bootfs
# (ext4) and no ESP at all. This file used to hardcode the first pair, which put
# one machine's partition table inside a distro-layer script - and made it
# silently unable to work on any other board. Both values now come from
# meta-med-bsp, keyed on machine; this script only knows that a key store exists.
#
# It is mounted privately here rather than through its normal mountpoint because
# local-fs.target is ordered after cryptsetup, so a unit that must run *before*
# cryptsetup cannot depend on that mount without a dependency cycle.
KEY_STORE=@MED_KEY_STORE_DEV@
KEY_STORE_FSTYPE=@MED_KEY_STORE_FSTYPE@
KEY_STORE_MNT=/run/med-provision-keystore
KEY_REL=medplatform/data.key

log() { echo "med-data-provision: $*"; }
fail() { echo "med-data-provision: $*" >&2; exit 1; }

is_mounted() { grep -q " $1 " /proc/mounts; }

cleanup() {
    if is_mounted "$KEY_STORE_MNT"; then
        umount "$KEY_STORE_MNT" || true
    fi
    rmdir "$KEY_STORE_MNT" 2>/dev/null || true
}
trap cleanup EXIT

mount_key_store() {
    [ -n "$KEY_STORE" ] || fail "no key store is declared for this machine \
(MED_KEY_STORE_DEV is empty). A development key needs a partition outside the \
A/B slots to live on; declare one in meta-med-bsp for this MACHINE."
    mkdir -p "$KEY_STORE_MNT"
    is_mounted "$KEY_STORE_MNT" && return 0
    mount -t "$KEY_STORE_FSTYPE" "$KEY_STORE" "$KEY_STORE_MNT" \
        || fail "cannot mount the key store ($KEY_STORE, $KEY_STORE_FSTYPE)"
}

# Opening is the same operation whether the volume was provisioned a second ago
# or three years ago, so both paths go through here.
open_volume() {
    [ -b "/dev/mapper/$MAPPER" ] && { log "already open"; return 0; }
    case "$KEY_SOURCE" in
    development)
        mount_key_store
        [ -f "$KEY_STORE_MNT/$KEY_REL" ] || fail "volume key missing at \
$KEY_STORE:/$KEY_REL - the volume is provisioned but its key is gone, and /data \
cannot be recovered without it"
        cryptsetup open --key-file "$KEY_STORE_MNT/$KEY_REL" "$DEV" "$MAPPER"
        umount "$KEY_STORE_MNT"
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

mount_key_store
mkdir -p "$(dirname "$KEY_STORE_MNT/$KEY_REL")"
KEYFILE="$KEY_STORE_MNT/$KEY_REL"

if [ ! -f "$KEYFILE" ]; then
    log "generating a device-unique volume key"
    # No status=none: dd here is busybox, which accepts only
    # if/of/bs/count/skip/seek and exits 1 on anything else.
    dd if=/dev/urandom of="$KEYFILE" bs=32 count=1 2>/dev/null
    # Best-effort by necessity: vfat carries no POSIX modes at all, so on an EFI
    # key store this silently does nothing and the key is readable by anyone who
    # mounts the medium. On an ext4 key store it does take effect. Either way
    # the threat model is the same, and is what makes this a development key: it
    # protects against removal of the medium, not against root on a live device.
    chmod 0400 "$KEYFILE" 2>/dev/null || true
fi

log "formatting $DEV as LUKS2"
cryptsetup luksFormat --type luks2 --batch-mode --key-file "$KEYFILE" "$DEV"
cryptsetup open --key-file "$KEYFILE" "$DEV" "$MAPPER"
mkfs.ext4 -F -q -L "$MAPPER" "/dev/mapper/$MAPPER"
umount "$KEY_STORE_MNT"

log "provisioned; /data is now backed by a dm-crypt mapping"
