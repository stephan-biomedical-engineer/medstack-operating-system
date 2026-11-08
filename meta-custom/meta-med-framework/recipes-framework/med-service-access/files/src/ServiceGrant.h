// SPDX-License-Identifier: MIT
//
// A service grant: the signed permission that opens one window of service
// access to a MedOS device (ACE-03 of docs/CONFORMIDADE.md; IEC 81001-5-1
// 5.3.2 g)-h): access that is not removed is documented and protected).
//
// The grant is a CMS SignedData (DER, content attached) over a short text:
//
//   med-service-grant 1
//   compatible med-os-qemux86-64
//   duration 1800
//   nonce 6f1c0a9e5b2d4c7a8e3f1b0d9c6a5e4f
//   key ssh-ed25519 AAAAC3Nz... technician@bench
//   reason replace the front-end board
//
// It is verified against the device's UPDATE keyring - the same root CA and
// CRL RAUC checks a bundle against, with the same rules (codeSigning extended
// key usage, a CRL for every certificate in the chain) - so the device has one
// trust anchor, and whoever may update it is whoever may service it. Nothing
// here trusts the device's clock beyond what OpenSSL does with certificate
// validity: the window is a DURATION counted from acceptance, never a date,
// because a board without an initialised RTC does not know the date.
//
// Pure libcrypto and the standard library, so the host suite
// (tests/service-access) exercises exactly what the device runs.

#pragma once

#include <set>
#include <string>

namespace med::service {

constexpr unsigned kMinDurationS = 60;
constexpr unsigned kMaxDurationS = 8 * 3600;
constexpr std::size_t kMaxGrantBytes = 64 * 1024;

struct Grant {
    std::string compatible;
    unsigned durationS = 0;
    std::string nonce;
    std::string keyType;
    std::string keyBlob;      // base64, as in authorized_keys
    std::string keyComment;
    std::string reason;
    // Filled by verifyGrant, not by the text.
    std::string signer;       // subject of the signing certificate
    std::string fingerprint;  // SHA256:<base64>, as ssh-keygen -l prints it
};

struct Verdict {
    bool ok = false;
    std::string reason;  // why it was refused; empty when ok
    Grant grant;
};

/// Parse the signed text. Strict: one value per key, no unknown key, no
/// control character, and a key line that can carry no authorized_keys
/// options (it is rewritten, never copied).
Verdict parseGrant(const std::string& text);

/// Verify the CMS signature against a PEM keyring (CA certificates and CRLs,
/// as RAUC's keyring file holds them), then parse what was signed.
Verdict verifyGrant(const std::string& der, const std::string& keyringPem);

/// The device-side checks: the grant names this device's compatible, and its
/// nonce was never used before (a grant opens one window, once).
Verdict checkGrant(const Verdict& verified, const std::string& compatible,
                   const std::set<std::string>& usedNonces);

/// The authorized_keys line the device writes: the key, with every forwarding
/// turned off ("restrict") and a terminal allowed back ("pty").
std::string authorizedKeysLine(const Grant& grant);

/// SHA256:<unpadded base64> of a base64 key blob; empty if it does not decode.
std::string keyFingerprint(const std::string& blobBase64);

}  // namespace med::service
