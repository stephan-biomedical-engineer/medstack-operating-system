// SPDX-License-Identifier: MIT
//
// The clinical file of an EEG session: BDF+, derived from the session record.
//
// implementation_plan_bdf.md is the specification; the decisions it names
// (D1..D10) are referred to by number below. In one paragraph: raw.bin stays
// the record, and the BDF+ is derived from it, deterministically, losslessly -
// every sample is checked to be a whole number of converter steps at the scale
// the session declared, and a session where one is not is refused whole. A
// session with gaps becomes one continuous file per uninterrupted stretch,
// because the format's discontinuous variant is read wrongly by common readers
// (measured, phase 2). Everything the record knows about itself that a sample
// cannot carry - the link, the montage, the clock, a gap, an interruption,
// electrode contact - becomes an annotation.
//
// No system call and no library beyond the MedFramework, like the service
// beside it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <medplatform/MedicalStorage.h>
#include <medplatform/MedicalTypes.h>

namespace eeg {

/// What metadata.json says about a session, and nothing it does not say: every
/// field the conversion depends on is required, and a session written before
/// the record declared its scale is refused rather than given one (D7).
struct SessionMetadata {
    std::string session;          ///< also the UTC start, "YYYY-MM-DDTHH:MM:SS.mmmZ"
    std::string deviceId;
    std::string link;
    std::string softwareVersion;
    std::string unit;
    std::uint16_t channels = 0;
    double sampleRateHz = 0.0;
    double fullScaleUv = 0.0;
    int resolutionBits = 0;
    std::string montage;          ///< "bipolar" or "referential"
    std::vector<std::string> electrodes;  ///< empty: none declared
    std::string reference;        ///< empty: none declared
    std::string prefiltering;     ///< empty: unknown
    std::string testSignal;       ///< the prescription's afe.test_signal, if any
    /// What every channel's inputs were switched to: "normal" (the
    /// electrodes) or "shorted", a bench measurement of the converter's own
    /// noise. A record written before the key existed measured electrodes.
    std::string input = "normal";
    /// The one channel that is not a voltage of interest, counted from 1, or
    /// 0 for none: its label, and what its samples mean -
    /// value = (uV - uvAtReference) / uvPerUnit + auxiliaryReference, in
    /// auxiliaryUnit. The other channels are what `input` says.
    unsigned auxiliaryChannel = 0;
    std::string auxiliaryLabel;
    std::string auxiliaryUnit = "uV";
    double uvPerUnit = 1.0;
    double uvAtReference = 0.0;
    double auxiliaryReference = 0.0;
    bool clockSynchronized = false;
    std::string utcOffset;        ///< "+HH:MM"
};

med::Result<SessionMetadata> parseMetadata(const std::string& json);

/// One clinical file written for a session.
struct ExportedFile {
    std::string name;
    std::string sha256;
    std::uint64_t records = 0;
};

struct ExportSummary {
    std::vector<ExportedFile> files;
    std::uint64_t frames = 0;        ///< valid frames written
    std::uint64_t failedCrc = 0;     ///< frames found but not trusted
    std::uint64_t missing = 0;       ///< frames the sequence says never arrived
    std::uint64_t restarts = 0;      ///< sequence went backwards: gap unknown
    std::uint64_t partialBytes = 0;  ///< a truncated frame at the end
    bool interrupted = false;
};

/// Derives the clinical file(s) of the session in `sessionDir`, relative to
/// `storage`. `interrupted` says the session has no close marker (D5) - the
/// caller knows, from the live marker, that it is not being written.
///
/// Fails with IntegrityError when the record does not match its declared
/// scale, with InvalidArgument when the metadata cannot be used, and with the
/// storage's status on an I/O error. Nothing is left that claims to be
/// complete in any of those cases: a file is complete only with its sidecar.
med::Result<ExportSummary> exportSession(med::MedicalStorage& storage,
                                         const std::string& sessionDir, bool interrupted);

/// export.json, the marker that a session has been dealt with - converted, or
/// refused with its reason - so that it is not attempted again.
std::string exportMarker(const ExportSummary& summary);
std::string refusalMarker(const std::string& reason);

}  // namespace eeg
