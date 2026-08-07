// SPDX-License-Identifier: MIT
//
// MedFramework - shared vocabulary implementation and library identity.

#include "MedicalTypes.h"

#include <openssl/evp.h>

#include <cstdio>
#include <ctime>

#include "MedDigest.h"

#ifndef MED_FRAMEWORK_VERSION
#define MED_FRAMEWORK_VERSION "0.0.0-dev"
#endif

namespace med {

const char* toString(Status status) noexcept {
    switch (status) {
        case Status::Ok:               return "OK";
        case Status::InvalidArgument:  return "INVALID_ARGUMENT";
        case Status::NotFound:         return "NOT_FOUND";
        case Status::PermissionDenied: return "PERMISSION_DENIED";
        case Status::Unavailable:      return "UNAVAILABLE";
        case Status::Timeout:          return "TIMEOUT";
        case Status::IoError:          return "IO_ERROR";
        case Status::IntegrityError:   return "INTEGRITY_ERROR";
        case Status::OutOfRange:       return "OUT_OF_RANGE";
        case Status::NotSupported:     return "NOT_SUPPORTED";
        case Status::Internal:         return "INTERNAL";
    }
    return "UNKNOWN";
}

std::uint64_t toUnixMicros(Timestamp timestamp) noexcept {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            timestamp.time_since_epoch())
                            .count();
    return micros < 0 ? 0U : static_cast<std::uint64_t>(micros);
}

Timestamp fromUnixMicros(std::uint64_t micros) noexcept {
    return Timestamp(std::chrono::microseconds(static_cast<std::int64_t>(micros)));
}

std::string formatTimestamp(Timestamp timestamp) {
    const std::uint64_t micros = toUnixMicros(timestamp);
    const std::time_t seconds = static_cast<std::time_t>(micros / 1000000ULL);
    const unsigned millis = static_cast<unsigned>((micros % 1000000ULL) / 1000ULL);

    std::tm broken{};
    // UTC only: a device that travels between time zones, or whose operator
    // changes one, must not produce an audit trail that appears to go
    // backwards in time.
    if (gmtime_r(&seconds, &broken) == nullptr) {
        return {};
    }

    char buffer[40];
    const std::size_t used = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &broken);
    if (used == 0) {
        return {};
    }
    std::snprintf(buffer + used, sizeof(buffer) - used, ".%03uZ", millis);
    return std::string(buffer);
}

const char* frameworkVersion() noexcept {
    return MED_FRAMEWORK_VERSION;
}

namespace internal {

std::string sha256Hex(const void* data, std::size_t length) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return {};
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLength = 0;
    const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(context, data, length) == 1 &&
                    EVP_DigestFinal_ex(context, digest, &digestLength) == 1;
    EVP_MD_CTX_free(context);

    if (!ok) {
        return {};
    }

    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(digestLength) * 2);
    for (unsigned int i = 0; i < digestLength; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

}  // namespace internal

}  // namespace med
