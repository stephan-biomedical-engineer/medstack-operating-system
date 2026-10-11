// SPDX-License-Identifier: MIT
//
// MedFramework - secure persistence of patient and biomedical records.
//
// Backed by the encrypted /data volume that meta-med-distro provisions (LUKS,
// unlocked from /etc/crypttab, mounted by data.mount). Applications never open
// paths under /data themselves; going through this API is what guarantees the
// three properties a record store for a medical device has to have:
//
//   * Confidentiality by construction. open() refuses a backing store that is
//     not a dm-crypt device unless the caller explicitly opts out, so a
//     misconfigured unit fails loudly at start-up instead of writing patient
//     data to a plaintext partition.
//   * Atomicity. A write is a temporary file, an fsync, a rename and an fsync
//     of the directory. A power loss leaves either the old record or the new
//     one, never a half written one.
//   * Containment. Relative paths only; "..", absolute paths and symlink
//     escapes are rejected before any syscall touches the filesystem.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MedicalTypes.h"

namespace med {

struct StorageConfig {
    /// Mountpoint of the persistent volume.
    std::string root = "/data";

    /// Subdirectory under `root` owned by this application, created on open().
    std::string nameSpace = "records";

    /// Refuse to operate on a backing device that is not dm-crypt. Development
    /// images (QEMU, no LUKS partition) set this to false deliberately and the
    /// decision is audit-logged by the caller.
    bool requireEncryptedBacking = true;
};

struct StorageInfo {
    std::string root;
    bool mounted = false;    ///< root is a mountpoint of its own, not rootfs
    bool encrypted = false;  ///< backing block device is a dm-crypt mapping
    std::uint64_t totalBytes = 0;
    std::uint64_t availableBytes = 0;
};

class MedicalStorage {
public:
    ~MedicalStorage();

    /// Validates the backing store, creates the namespace directory and
    /// returns a handle. PermissionDenied when encryption is required and the
    /// backing device is not encrypted; Unavailable when /data is not mounted.
    static Result<std::unique_ptr<MedicalStorage>> open(const StorageConfig& config);

    Status write(const std::string& relativePath, const void* data, std::size_t length);
    Status writeString(const std::string& relativePath, const std::string& content);

    /// Append to a record without rewriting it. Used by long acquisitions,
    /// where rewriting a growing session file on every frame is not viable.
    /// Each append is fsync'd: the cost buys a bounded loss window on power
    /// failure.
    Status append(const std::string& relativePath, const void* data, std::size_t length);

    Result<std::vector<std::uint8_t>> read(const std::string& relativePath) const;
    Result<std::string> readString(const std::string& relativePath) const;

    /// At most `length` bytes from `offset`, for records too large to hold in
    /// memory - hours of acquisition. Fewer bytes than asked for means the end
    /// of the file was reached, and none means `offset` is at or past it.
    Result<std::vector<std::uint8_t>> read(const std::string& relativePath, std::uint64_t offset,
                                           std::size_t length) const;

    /// Size in bytes of a regular file.
    Result<std::uint64_t> size(const std::string& relativePath) const;

    bool exists(const std::string& relativePath) const;
    Status remove(const std::string& relativePath);

    /// Makes ONE finished file readable to the members of `group`, and nothing
    /// else with it.
    ///
    /// Everything this class writes is 0600 under 0700 directories: only the
    /// identity that wrote a record can reach it, by any path. That is right
    /// for the record and wrong for exactly one case - a derived file that
    /// exists to be read by someone with less privilege, such as a clinical
    /// file handed to a reviewing program that must not see the raw record
    /// beside it.
    ///
    /// The file becomes 0640, group `group`, and gains a second name:
    /// `<root>/review/<namespace>/<publishedName>`, in a tree that is
    /// root:`group` 0750. A hard link and not a copy - same volume, same inode,
    /// no second set of the patient's bytes to keep in step or to forget. The
    /// file's first name stays where it was, behind the 0700 directories, so
    /// the namespace is exactly as closed as before: the group can list the
    /// review tree and open what is in it, and has no path to anything else.
    ///
    /// Idempotent. Publishing a file again is Ok; publishing a NEW file under
    /// a name already taken replaces the name, which is what a re-derived file
    /// needs. The caller decides WHEN a file is finished - this API cannot
    /// know, and a file still being written must not be published.
    ///
    /// InvalidArgument: an unsafe path, a name with a separator in it, or a
    /// source that is not a regular file. NotFound: no such source.
    /// Unavailable: no such group on this system - an image that ships no
    /// reviewer has no reviewing identity, and then nothing is published.
    /// NotSupported: this handle has no namespace to publish from.
    Status publish(const std::string& relativePath, const std::string& publishedName,
                   const std::string& group);

    /// Removes a published name. The file itself, under its first name, is
    /// untouched. NotFound when it was not published.
    Status unpublish(const std::string& publishedName);

    /// Entries of a directory relative to the namespace root, sorted. Not
    /// recursive.
    Result<std::vector<std::string>> list(const std::string& relativeDir = "") const;

    Result<StorageInfo> info() const;

    /// Absolute path of the namespace root. For diagnostics and audit fields
    /// only - callers must not build paths from it.
    const std::string& basePath() const noexcept;

    MedicalStorage(const MedicalStorage&) = delete;
    MedicalStorage& operator=(const MedicalStorage&) = delete;

private:
    explicit MedicalStorage(std::string basePath, StorageInfo info);

    std::string basePath_;
    StorageInfo info_;
};

// ---------------------------------------------------------------------------
// Biosignal files: BDF+.
//
// The exchange format clinical and research software opens for recorded
// biopotentials - EEG, ECG, EMG, polysomnography - in its 24-bit variant, so
// that a 24-bit converter's codes are carried without loss. This is a writer of
// the FORMAT and of nothing else: it knows signals, labels, physical
// dimensions and annotations, and has no idea what an electrode, a montage or
// a lead is. Those words belong to the device that fills the fields.
//
// Constants checked against the EDF, EDF+ and BDF+ specifications
// (implementation_plan_bdf.md, phase 0); the rules the writer enforces are the
// ones a reader relies on, so a header it cannot write correctly is refused,
// never approximated silently.
//
// A file is complete when, and only when, its "<path>.sha256" sidecar exists.
// The data is appended as it is produced - an hours-long recording does not
// fit in memory - so a power loss leaves a partial file with no sidecar, and
// whoever finds one starts again: create() removes both before writing.
// ---------------------------------------------------------------------------

/// One ordinary signal. Every text is printable ASCII, left-justified in a
/// fixed field; create() refuses one that does not fit rather than cutting it,
/// because a truncated label can be another signal's label.
struct BiosignalSignal {
    std::string label;              ///< 16 characters
    std::string transducer;         ///< 80, e.g. "AgAgCl electrode"; may be empty
    std::string physicalDimension;  ///< 8, e.g. "uV"
    double physicalMinimum = 0.0;   ///< the physical value of digitalMinimum
    double physicalMaximum = 0.0;   ///< the physical value of digitalMaximum
    std::int32_t digitalMinimum = -8388608;
    std::int32_t digitalMaximum = 8388607;
    std::string prefiltering;       ///< 80, e.g. "HP:DC LP:65Hz"
};

struct BiosignalHeader {
    /// EDF+ local patient identification, its subfields already composed by
    /// the caller ("X X X X" when anonymous). 80 characters.
    std::string patient;

    /// EDF+ local recording identification AFTER the start date: the writer
    /// composes "Startdate DD-MMM-YYYY " from the start below and appends
    /// this. 80 characters in all.
    std::string recording;

    /// Start of the recording in LOCAL time, which is what EDF defines -
    /// subsecond included. Years 1985 to 2084, the range the format's
    /// two-digit date can express.
    int year = 0;
    int month = 0;   ///< 1..12
    int day = 0;     ///< 1..31
    int hour = 0;
    int minute = 0;
    int second = 0;
    double startFraction = 0.0;  ///< [0, 1): the first record's onset

    double recordDurationSeconds = 0.0;  ///< > 0; need not be a whole number
    std::uint32_t samplesPerRecord = 0;  ///< per signal, the same for all
    std::vector<BiosignalSignal> signals;

    /// BDF+C when true: every record starts where the previous one ended.
    /// BDF+D when false: records carry their own onsets, and gaps are visible.
    bool continuous = true;

    /// Room for annotations in each record, in bytes. The first annotation of
    /// every record is the record's own onset; the rest is for events.
    std::uint32_t annotationBytesPerRecord = 0;

    /// Known before the first byte is written: the header states it.
    std::uint64_t recordCount = 0;
};

struct BiosignalAnnotation {
    /// Seconds from the header's start SECOND, as EDF+ defines it - so an
    /// event at the first sample is at startFraction, not at 0. Negative and
    /// earlier onsets are legal (a pre-interval stimulus), but readers measure
    /// from the first sample and MNE drops anything before it.
    double onsetSeconds = 0.0;
    double durationSeconds = -1.0;  ///< negative: an instant, no duration
    std::string text;               ///< UTF-8, no control characters
};

class BiosignalWriter {
public:
    ~BiosignalWriter();

    /// Validates the whole header, removes any previous file and sidecar at
    /// `relativePath`, and writes the header.
    static Result<std::unique_ptr<BiosignalWriter>> create(MedicalStorage& storage,
                                                            const std::string& relativePath,
                                                            const BiosignalHeader& header);

    /// One data record. `samples` holds samplesPerRecord samples of every
    /// signal, INTERLEAVED - sample i of signal s at i * signals + s - which
    /// is how acquisition delivers them; the writer lays them out per signal,
    /// which is how the format stores them.
    ///
    /// The onset is checked, not trusted: in a continuous file it must be
    /// startFraction + index * duration, and in a discontinuous one it must
    /// not overlap the previous record. A sample outside its signal's digital
    /// range is refused (OutOfRange): the format cannot represent it, and
    /// clipping it would change the record.
    ///
    /// Annotations are written in this record when they fit; one that does
    /// not is carried, whole, into the next. One that could never fit in any
    /// record is refused at once.
    Status appendRecord(double onsetSeconds, const std::int32_t* samples,
                        const std::vector<BiosignalAnnotation>& annotations = {});

    /// Checks that recordCount records were written and nothing is still
    /// waiting for room, flushes, and writes the sidecar. Until this returns
    /// Ok the file is not complete.
    Status finish();

    /// Lowercase hex SHA-256 of the file, once finish() succeeded.
    const std::string& digest() const noexcept;

    /// Bytes an annotation takes in a record, and bytes the time-keeping
    /// annotation of a record starting at `onsetSeconds` takes - the writer's
    /// own encoding, so that a caller sizing annotationBytesPerRecord does not
    /// restate the format and drift from it.
    static std::size_t annotationBytes(const BiosignalAnnotation& annotation);
    static std::size_t timekeepingBytes(double onsetSeconds);

    BiosignalWriter(const BiosignalWriter&) = delete;
    BiosignalWriter& operator=(const BiosignalWriter&) = delete;

private:
    struct Impl;
    explicit BiosignalWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace med
