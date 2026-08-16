#!/bin/sh
#
# MedPlatform update-signing PKI.
#
# Builds the X.509 material RAUC needs to verify a bundle:
#
#   MedPlatform Development Root CA      -> shipped on the device as the keyring
#     `- MedPlatform Bundle Signing      -> signs bundles, never leaves the host
#   + a certificate revocation list       -> also shipped, see CRL note below
#
# WHY TWO LEVELS AND NOT A SELF-SIGNED CERTIFICATE
# A self-signed signing certificate cannot be rotated: replacing it means
# replacing the keyring on every unit in the field, which is exactly the
# operation a device installed in a hospital cannot be relied on to survive.
# With a root CA in the keyring, the signing certificate can be reissued or
# revoked without touching deployed devices.
#
# WHAT IS AND IS NOT IN THE REPOSITORY
# Only the keyring - CA certificate plus CRL - is committed. It is public by
# construction, because it is installed into the rootfs. Every private key
# stays under pki/, which is gitignored. A fresh clone therefore cannot sign a
# bundle, which is the correct default: the ability to produce an update that
# devices will accept is not repository content.
#
# THIS IS A DEVELOPMENT CA. For a real product the root key belongs in an HSM,
# generated in a witnessed ceremony, never present on a build host. The
# structure below is the same one; only the custody of the key differs, and the
# thesis should say so rather than imply this is shippable as is.
#
# THREE SETTINGS THAT HAVE TO AGREE (all three, or bundle verification fails)
#   1. The signing certificate carries extendedKeyUsage=codeSigning.
#   2. system.conf sets [keyring] check-purpose=codesign, so RAUC installs its
#      own X509 purpose callback. Without it OpenSSL's CMS code falls back to
#      the smime_sign purpose, which *rejects* a certificate whose EKU is
#      codeSigning - the strict certificate would be the reason for the
#      failure.
#   3. system.conf sets check-crl=true, which makes OpenSSL demand a CRL for
#      every certificate in the chain. That is why the keyring below is the CA
#      certificate *concatenated with* a CRL rather than the certificate alone.
#
# CRL NOTE
# The CRL is issued with a 20 year nextUpdate. On a fleet with network access
# that would be wrong - a CRL is supposed to be refreshed - but a device that
# may be offline for months and whose RTC may be wrong cannot fail closed on an
# expired CRL without becoming unupdatable, which is its own safety problem.
# The PoC therefore keeps revocation *possible* (revoke, regenerate, ship the
# new keyring in the next bundle) and documents the compromise instead of
# pretending an online CRL distribution point exists.
#
# Usage:
#   ./scripts/med-pki.sh            generate (refuses to clobber an existing CA)
#   ./scripts/med-pki.sh --force    regenerate from scratch
#   make pki                        the same thing

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKI="$ROOT/pki"
LAYER_KEYRING="$ROOT/meta-custom/meta-med-distro/recipes-core/rauc/rauc-conf/med-keyring.pem"

# EC P-256 rather than RSA: verification happens on the device, on a Cortex-A35,
# at every boot that follows an update. A P-256 signature is ~64 bytes against
# ~512 for RSA-4096 and verifies far faster, and OpenSSL's CMS implementation -
# which is what RAUC uses - supports it.
CURVE="prime256v1"

CA_DAYS=7300      # 20 years: a medical device outlives its build host.
SIGNING_DAYS=1825 # 5 years: short enough that rotation gets exercised.
CRL_DAYS=7300     # see CRL NOTE above.

command -v openssl >/dev/null || { echo "error: openssl not found" >&2; exit 1; }

# Refresh the CRL and the keyring after a revocation, without regenerating any
# key. This is the operation an actual revocation needs; --force is not.
if [ "${1:-}" = "--regen-crl" ]; then
    [ -f "$PKI/openssl.cnf" ] || { echo "error: no CA at $PKI - run without arguments first" >&2; exit 1; }
    openssl ca -config "$PKI/openssl.cnf" -gencrl -crldays "$CRL_DAYS" \
        -out "$PKI/ca/med-ca.crl.pem"
    cat "$PKI/ca/med-ca.cert.pem" "$PKI/ca/med-ca.crl.pem" > "$PKI/ca/med-keyring.pem"
    cp "$PKI/ca/med-keyring.pem" "$LAYER_KEYRING"
    echo "keyring refreshed: $LAYER_KEYRING"
    echo "Revoked certificates only take effect on a device once this keyring"
    echo "reaches it - which means shipping it in a bundle signed by a key that"
    echo "is still valid. Revoking the only signing key strands the fleet."
    exit 0
fi

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ -f "$PKI/ca/med-ca.key.pem" ] && [ "$FORCE" -eq 0 ]; then
    echo "error: $PKI/ca/med-ca.key.pem already exists." >&2
    echo "Regenerating the root CA invalidates every device already carrying" >&2
    echo "the old keyring. Pass --force if that is genuinely what you want." >&2
    exit 1
fi

rm -rf "$PKI"
mkdir -p "$PKI/ca/newcerts" "$PKI/signing"
chmod 700 "$PKI"
: > "$PKI/ca/index.txt"
echo 01 > "$PKI/ca/serial"
echo 01 > "$PKI/ca/crlnumber"

# A real CA database, not just loose files: "openssl ca" needs it, and it is
# what makes revocation a recorded operation rather than a deletion.
cat > "$PKI/openssl.cnf" <<EOF
[ ca ]
default_ca = med_ca

[ med_ca ]
dir               = $PKI
database          = \$dir/ca/index.txt
new_certs_dir     = \$dir/ca/newcerts
certificate       = \$dir/ca/med-ca.cert.pem
private_key       = \$dir/ca/med-ca.key.pem
serial            = \$dir/ca/serial
crlnumber         = \$dir/ca/crlnumber
default_md        = sha256
policy            = med_policy
email_in_dn       = no
unique_subject    = no
copy_extensions   = none
default_days      = $SIGNING_DAYS
default_crl_days  = $CRL_DAYS

[ med_policy ]
organizationName       = supplied
organizationalUnitName = optional
commonName             = supplied

[ signing_cert ]
basicConstraints       = critical,CA:FALSE
keyUsage               = critical,digitalSignature
extendedKeyUsage       = critical,codeSigning
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid,issuer
EOF

echo "==> root CA (EC $CURVE, ${CA_DAYS}d)"
openssl ecparam -name "$CURVE" -genkey -noout -out "$PKI/ca/med-ca.key.pem"
chmod 600 "$PKI/ca/med-ca.key.pem"

# No extendedKeyUsage on the CA on purpose. RAUC's codesign check reads it as
# "if the CA declares an EKU it must include codeSigning"; leaving it absent
# keeps the root usable if the platform ever needs to sign something else,
# without weakening the leaf check.
openssl req -x509 -new -key "$PKI/ca/med-ca.key.pem" -sha256 -days "$CA_DAYS" \
    -out "$PKI/ca/med-ca.cert.pem" \
    -subj "/O=MedPlatform/OU=Update Infrastructure/CN=MedPlatform Development Root CA" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash"

echo "==> bundle signing certificate (EC $CURVE, ${SIGNING_DAYS}d)"
openssl ecparam -name "$CURVE" -genkey -noout -out "$PKI/signing/med-signing.key.pem"
chmod 600 "$PKI/signing/med-signing.key.pem"

openssl req -new -key "$PKI/signing/med-signing.key.pem" \
    -out "$PKI/signing/med-signing.csr.pem" \
    -subj "/O=MedPlatform/OU=Update Infrastructure/CN=MedPlatform Bundle Signing"

openssl ca -batch -config "$PKI/openssl.cnf" \
    -extensions signing_cert -days "$SIGNING_DAYS" -notext \
    -in "$PKI/signing/med-signing.csr.pem" \
    -out "$PKI/signing/med-signing.cert.pem"
rm -f "$PKI/signing/med-signing.csr.pem"

echo "==> certificate revocation list (${CRL_DAYS}d)"
openssl ca -config "$PKI/openssl.cnf" -gencrl -crldays "$CRL_DAYS" \
    -out "$PKI/ca/med-ca.crl.pem"

# The keyring RAUC loads is one PEM file holding both objects: OpenSSL's
# X509_load_cert_crl_file reads every PEM block, so the CA certificate and the
# CRL can live in the same file and check-crl=true is satisfied offline.
cat "$PKI/ca/med-ca.cert.pem" "$PKI/ca/med-ca.crl.pem" > "$PKI/ca/med-keyring.pem"

echo "==> verifying the chain the way the device will"
openssl verify -CAfile "$PKI/ca/med-keyring.pem" -crl_check_all \
    "$PKI/signing/med-signing.cert.pem"

cp "$PKI/ca/med-keyring.pem" "$LAYER_KEYRING"

echo
echo "keyring (public, committed):  $LAYER_KEYRING"
echo "root key  (private, ignored): $PKI/ca/med-ca.key.pem"
echo "sign key  (private, ignored): $PKI/signing/med-signing.key.pem"
echo
echo "Sign a bundle with:"
echo "  rauc bundle --cert=$PKI/signing/med-signing.cert.pem \\"
echo "              --key=$PKI/signing/med-signing.key.pem  <input-dir> <bundle.raucb>"
echo
echo "Revoke the signing certificate with:"
echo "  openssl ca -config $PKI/openssl.cnf -revoke $PKI/signing/med-signing.cert.pem"
echo "  ./scripts/med-pki.sh --regen-crl   # then ship the new keyring in a bundle"
