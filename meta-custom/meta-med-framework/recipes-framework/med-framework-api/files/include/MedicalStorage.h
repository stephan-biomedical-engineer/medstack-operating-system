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

    bool exists(const std::string& relativePath) const;
    Status remove(const std::string& relativePath);

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

}  // namespace med
