// SPDX-License-Identifier: MIT
//
// MedFramework - shared vocabulary types.
//
// Every MedFramework API reports failure through Status / Result<T> instead of
// exceptions or errno: a medical application must be able to handle an error
// at the call site and log it, and an exception escaping an acquisition loop
// is a hazard, not a diagnostic.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace med {

/// Outcome of a MedFramework operation.
enum class Status : int {
    Ok = 0,
    InvalidArgument,  ///< caller passed something the API cannot honour
    NotFound,         ///< the addressed record / key / slot does not exist
    PermissionDenied, ///< refused by policy (e.g. unencrypted storage backend)
    Unavailable,      ///< the underlying resource is not present or not ready
    Timeout,          ///< no data within the caller supplied deadline
    IoError,          ///< the kernel or the peer failed the transfer
    IntegrityError,   ///< data exists but failed verification
    OutOfRange,       ///< a value violates a declared safety limit
    NotSupported,     ///< valid request, not implemented by this backend
    Internal,         ///< framework defect - always audit-logged
    /// The peer is not keeping up: the operation would have blocked and was
    /// refused instead.
    ///
    /// Distinct from Timeout on purpose, and the distinction is the difference
    /// between two very different clinical situations. Timeout means nothing
    /// arrived in the window the caller allowed. WouldBlock means the caller is
    /// producing faster than the far side consumes - the far side is present
    /// and healthy, it is just slow. A producer that treats the second as an
    /// error drops a working consumer; one that treats it as backpressure lets
    /// a display decide how fast a patient is sampled. Neither is acceptable,
    /// so the status says which one happened.
    ///
    /// Appended after Internal rather than inserted, so no existing enumerator
    /// changes value.
    WouldBlock,
};

/// Stable, machine-parsable spelling of a Status, used as a journal field.
const char* toString(Status status) noexcept;

inline bool isOk(Status status) noexcept { return status == Status::Ok; }

using Clock = std::chrono::system_clock;
using Timestamp = Clock::time_point;

/// ISO-8601 UTC with millisecond resolution, e.g. "2026-08-06T12:34:56.789Z".
/// This is the timestamp format written into audit records and stored data.
std::string formatTimestamp(Timestamp timestamp);

/// Microseconds since the Unix epoch - the wire format for AMP sample frames.
std::uint64_t toUnixMicros(Timestamp timestamp) noexcept;

Timestamp fromUnixMicros(std::uint64_t micros) noexcept;

/// Version of the MedFramework the caller was linked against. Belongs in the
/// SystemStart audit record: a trail that does not name the software that
/// produced it is not traceable (IEC 62304 5.1.1).
const char* frameworkVersion() noexcept;

/// A value or the reason it could not be produced.
///
/// T must be default constructible; every type the framework returns is.
/// Result is move-only whenever T is (Result<std::unique_ptr<...>>).
template <typename T>
class Result {
public:
    Result() = default;

    static Result ok(T value) {
        Result result;
        result.status_ = Status::Ok;
        result.value_ = std::move(value);
        return result;
    }

    static Result fail(Status status, std::string message = {}) {
        Result result;
        // A failed Result must never carry Status::Ok, or callers testing
        // `if (r)` would take the success branch with an unset value.
        result.status_ = (status == Status::Ok) ? Status::Internal : status;
        result.message_ = std::move(message);
        return result;
    }

    bool isOk() const noexcept { return status_ == Status::Ok; }
    explicit operator bool() const noexcept { return isOk(); }

    Status status() const noexcept { return status_; }
    const std::string& message() const noexcept { return message_; }

    /// Only meaningful when isOk(); returns the default constructed value
    /// otherwise, so a caller that ignores the status still reads defined data.
    const T& value() const& noexcept { return value_; }
    T& value() & noexcept { return value_; }

    /// Move the value out. Leaves this Result in a valid but unspecified state.
    T take() { return std::move(value_); }

private:
    Status status_ = Status::Ok;
    T value_{};
    std::string message_;
};

}  // namespace med
