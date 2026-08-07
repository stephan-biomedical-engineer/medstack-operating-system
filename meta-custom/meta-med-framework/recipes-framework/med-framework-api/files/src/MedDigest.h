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

}  // namespace internal
}  // namespace med
