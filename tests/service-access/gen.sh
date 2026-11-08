#!/bin/sh
# Test PKI and grants for tests/service-access. Everything under $1; nothing
# here is the repository's PKI (pki/), and nothing leaves the build directory.
set -eu
B=$1
rm -rf "$B" && mkdir -p "$B/db"
cd "$B"
: > db/index.txt; echo 01 > db/serial; echo 01 > db/crlnumber
cat > ca.cnf <<CNF
[ca]
default_ca = test
[test]
dir = .
database = db/index.txt
serial = db/serial
crlnumber = db/crlnumber
new_certs_dir = db
default_md = sha256
default_days = 30
default_crl_days = 30
policy = any
copy_extensions = none
[any]
commonName = supplied
[root]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[codesign]
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
[noeku]
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature
[email]
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = emailProtection
CNF
q() { "$@" >/dev/null 2>&1; }
ca() { # name subject
    q openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$1.key" -out "$1.pem" \
        -subj "/CN=$2" -days 30 -config ca.cnf -extensions root
}
leaf() { # name ext [signing-ca]
    q openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$1.key" -out "$1.csr" -subj "/CN=$1"
    q openssl ca -batch -config ca.cnf -cert "${3:-root}.pem" -keyfile "${3:-root}.key" -extensions "$2" \
        -in "$1.csr" -out "$1.pem"
}
ca root "Test Root CA"
leaf good codesign
leaf noeku noeku
leaf email email
leaf revoked codesign
q openssl ca -config ca.cnf -cert root.pem -keyfile root.key -revoke revoked.pem
q openssl ca -config ca.cnf -cert root.pem -keyfile root.key -gencrl -out crl.pem
cat root.pem crl.pem > keyring.pem
cp root.pem keyring-nocrl.pem
# A stranger: a CA the device does not hold, and a codesign leaf under it.
: > db/index.txt
ca stranger "Stranger CA"
leaf foreign codesign stranger

ssh-keygen -q -t ed25519 -N '' -C tech@bench -f tech
ssh-keygen -lf tech.pub | cut -d' ' -f2 > tech.fingerprint
KEY=$(cut -d' ' -f1-3 tech.pub)
BLOB=$(cut -d' ' -f2 tech.pub)

grant() { # file, then lines after the header
    f=$1; shift
    { echo "med-service-grant 1"; for l in "$@"; do echo "$l"; done; } > "$f.txt"
}
N=0123456789abcdef0123456789abcdef
grant good "compatible med-os-test" "duration 1800" "nonce $N" "key $KEY" "reason bench test"
sign() { # text signer out
    q openssl cms -sign -binary -nodetach -outform DER -md sha256 -signer "$2.pem" -inkey "$2.key" \
        -in "$1.txt" -out "$3.p7s"
}
sign good good good
sign good noeku noeku
sign good email email
sign good revoked revoked
sign good foreign foreign
# One byte of the signed content changed, after signing.
python3 -c "
import sys; d=bytearray(open('good.p7s','rb').read()); i=d.find(b'bench test'); d[i]^=1
open('tampered.p7s','wb').write(d)"
cp good.txt plain.p7s
echo "$BLOB" > tech.blob
