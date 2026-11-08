#!/bin/sh
# Make a service grant for a MedOS production image, on a disk image that the
# device takes as a removable medium (ACE-03, docs/CONFORMIDADE.md).
#
#   scripts/med-service-grant.sh PUBKEY DURATION_S "REASON" [IMAGE]
#
# PUBKEY is the technician's SSH public key file. DURATION_S is how long the
# window stays open once the device accepts the grant (60..28800). IMAGE is the
# production image the grant is for (default med-image-eeg-prod): its RAUC
# compatible is read from the built rootfs, so a grant names exactly the
# devices that image is installed on. Signed with the update signing key in
# pki/signing (scripts/med-pki.sh) - the device verifies it against the same
# keyring it verifies an update bundle against.
#
# Output: build/service-grant.img, an ext4 image labelled med-service holding
# grant.p7s. On QEMU it is plugged in through the monitor as a USB stick
# (tests/conformity/service_access.py); on a board, write it to a stick.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PUB=$1; DUR=$2; REASON=$3; IMAGE=${4:-med-image-eeg-prod}
EXT4=$ROOT/build/tmp-glibc/deploy/images/qemux86-64/$IMAGE-qemux86-64.rootfs.ext4
CERT=$ROOT/pki/signing/med-signing.cert.pem
KEY=$ROOT/pki/signing/med-signing.key.pem
OUT=$ROOT/build/service-grant.img

[ -f "$PUB" ] || { echo "no public key at $PUB" >&2; exit 1; }
[ -f "$CERT" ] && [ -f "$KEY" ] || { echo "no signing key - run scripts/med-pki.sh" >&2; exit 1; }
COMPAT=$(debugfs -R "cat /etc/rauc/system.conf" "$(realpath "$EXT4")" 2>/dev/null \
         | sed -n 's/^compatible=//p')
[ -n "$COMPAT" ] || { echo "no compatible in $EXT4" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
NONCE=$(openssl rand -hex 16)
{
    echo "med-service-grant 1"
    echo "compatible $COMPAT"
    echo "duration $DUR"
    echo "nonce $NONCE"
    echo "key $(cut -d' ' -f1-3 "$PUB")"
    echo "reason $REASON"
} > "$STAGE/grant.txt"
mkdir "$STAGE/fs"
openssl cms -sign -binary -nodetach -outform DER -md sha256 \
    -signer "$CERT" -inkey "$KEY" -in "$STAGE/grant.txt" -out "$STAGE/fs/grant.p7s"
rm -f "$OUT" && truncate -s 8M "$OUT"
mkfs.ext4 -F -q -L med-service -d "$STAGE/fs" "$OUT"
echo "grant for $COMPAT, $DUR s, nonce $NONCE -> $OUT"
