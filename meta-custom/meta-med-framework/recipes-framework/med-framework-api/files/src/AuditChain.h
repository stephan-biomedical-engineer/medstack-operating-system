// SPDX-License-Identifier: MIT
//
// Internal: the audit hash chain's canonical form, and its verifier.
//
// One definition of "what a record's digest covers", used by MedicalLogger to
// write the chain and by med-audit-verify to replay it. Not installed.
//
// Why there is a format 2. Format 1 (until 2026-11-05) hashed
// seq|time|event|message|key=value... with the CALLER's keys in the CALLER's
// order. The journal keeps neither: it stores the normalised name
// (session_id -> MED_SESSION_ID, and that mapping has no inverse) and does not
// keep fields in the order they were sent. Measured on QEMU the same day, over
// 42 real records: 5 recomputed from the journal's field order, 3 from sorted
// order, 21 from any permutation of their fields. So the chain's LINKAGE
// (each PREV equal to the previous HASH) could be checked and its CONTENT could
// not - an edited field and a reordered one were indistinguishable. A tamper-
// evidence mechanism nobody can verify is a claim, not a mechanism.
//
// Format 2 hashes what the journal does keep, in an order the verifier can
// reproduce: MESSAGE and every MED_* field the entry carries, except
// MED_AUDIT_HASH itself, sorted by name, each as
//
//     NAME '=' <decimal length of value> ':' <value bytes> '\n'
//
// The length prefix makes the form unambiguous whatever a value contains (a
// '|', a '=', a newline). The digest is SHA-256 of PREV '\n' canonical. And the
// fields the framework adds - component, device, software version, format -
// are covered too: format 1 left the record's attribution outside the chain.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "MedicalTypes.h"

namespace med {
namespace audit {

constexpr const char* kFormat = "2";
/// PREV of the first record of a chain: "no predecessor", so a verifier can
/// tell a chain that starts from one that was cut.
extern const char* const kGenesis;
/// Set by parseExport on an entry in which a field appeared twice. Journal
/// field names beginning "__" are the journal's own and no client can send
/// one, so this cannot collide with a record's field.
extern const char* const kDuplicateMarker;

using Fields = std::map<std::string, std::string>;

/// True for a field the digest covers: MESSAGE and MED_* except MED_AUDIT_HASH.
bool covered(const std::string& name);

std::string canonical(const Fields& fields);

/// SHA-256 hex of prev '\n' canonical(fields); empty if hashing failed.
std::string digest(const std::string& prev, const Fields& fields);

/// Parse journalctl's export format (journal-export-formats(7)): fields as
/// NAME=value lines, or NAME, a little-endian 64-bit length and the raw bytes
/// for values the text form cannot carry; an empty line ends an entry. A
/// field repeated in one entry keeps its first value and is reported through
/// `duplicates` (a verifier must not pick one silently).
Result<std::vector<Fields>> parseExport(const std::string& data,
                                        std::uint64_t* duplicates = nullptr);

struct Break {
    std::string component;
    std::uint64_t sequence = 0;
    std::string what;
};

struct ComponentReport {
    std::string component;
    std::uint64_t records = 0;
    /// Format 2 records whose digest was recomputed and matched.
    std::uint64_t verified = 0;
    /// Format 1 records: linkage checked, content not checkable.
    std::uint64_t legacy = 0;
    /// A record with SEQ 1 and the genesis PREV after the chain had begun:
    /// the chain's persisted head was lost and it restarted. Legitimate (a
    /// first boot, a wiped state file) and reported, never a break - and never
    /// proof of anything either, since a forger could start one too.
    std::uint64_t restarts = 0;
    /// SEQ of the first record seen. Above 1 when the journal no longer holds
    /// the chain's beginning (rotation, vacuum): not a break.
    std::uint64_t firstSequence = 0;
};

struct Report {
    std::uint64_t entries = 0;
    std::vector<ComponentReport> components;
    std::vector<Break> breaks;
};

/// Verify every audit entry (MED_AUDIT=1) of `entries`, in the order given -
/// the journal's order. Chains are per MED_COMPONENT. A break is reported at
/// the record where it is seen, and verification goes on from that record, so
/// one edit is one break and not a cascade.
Report verify(const std::vector<Fields>& entries);

}  // namespace audit
}  // namespace med
