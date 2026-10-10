// SPDX-License-Identifier: MIT
//
// Internal helper: SHA-256 over OpenSSL's EVP interface.
// Not installed - applications get integrity through MedicalConfiguration and
// MedicalLogger rather than by hashing things themselves.

#pragma once

#include <cstddef>
#include <string>

namespace med {
namespace internal {

/// Lowercase hex SHA-256. Returns an empty string if OpenSSL fails, which
/// every caller treats as an integrity failure rather than as "no digest".
std::string sha256Hex(const void* data, std::size_t length);

inline std::string sha256Hex(const std::string& data) {
    return sha256Hex(data.data(), data.size());
}

/// Incremental SHA-256, for content too large to hold in memory at once - a
/// clinical file derived from hours of acquisition. Any OpenSSL failure is
/// sticky: finishHex() then returns an empty string, which callers treat as an
/// integrity failure, exactly like sha256Hex().
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const void* data, std::size_t length);

    /// Lowercase hex digest of everything given to update(). Once only.
    std::string finishHex();

private:
    void* context_;
    bool ok_;
};

}  // namespace internal
}  // namespace med
