// SPDX-License-Identifier: MIT
//
// MedFramework - operational parameters, calibration and safety limits.
//
// Holds the values that decide how a device behaves clinically: channel gains
// and calibration coefficients, alarm thresholds, acquisition rates. Two
// things separate this from "read an ini file":
//
//   * Integrity. The store is written together with a SHA-256 sidecar and
//     verified on load. A calibration table that was corrupted by a failing
//     eMMC, or edited outside the application, is detected as IntegrityError
//     instead of being silently used to scale patient measurements.
//   * Declared safety limits. validate() checks the loaded values against a
//     SafetyLimit table supplied by the application - the DERS (dose error
//     reduction) pattern generalised: hard limits live next to the values they
//     bound, and violating one is a reportable event, never a clamp.
//
// Changes are atomic (temporary file + rename + fsync) and are expected to be
// paired with an AuditEvent::ConfigurationChanged / CalibrationChanged record
// by the caller - the framework deliberately does not log on the caller's
// behalf, so the audit trail always names the responsible component.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "MedicalTypes.h"

namespace med {

/// A bound a configuration value must satisfy to be usable clinically.
///
/// The three predicates below are all "shapes a number may take", which is a
/// platform concern and belongs here. What a *particular* value must be, and
/// how one parameter constrains another, stays in the application.
struct SafetyLimit {
    std::string key;
    double minimum = 0.0;
    double maximum = 0.0;
    /// When true, absence of the key is itself a violation. Use for anything
    /// a safe start-up depends on.
    bool required = true;
    /// Human readable purpose, copied into the violation report and the audit
    /// record.
    std::string description;

    /// When non-empty, the value must be one of these exactly - a range check
    /// is still applied but is not sufficient.
    ///
    /// Real hardware rarely offers continuous settings. A converter whose
    /// output data rates are a fixed set of powers of two does not take a value
    /// between two of them, so a configuration asking for one is not a
    /// slightly-off request, it is an unimplementable one. Expressing that as a
    /// range would let it through and leave the driver to round - and a device
    /// running at a setting its own records do not name is a traceability
    /// defect, not a rounding convenience.
    std::vector<double> allowed;

    /// When > 0, the value must be an exact multiple of this - channels that
    /// only come in blocks, a buffer that only takes whole pages.
    double multipleOf = 0.0;
};

struct SafetyViolation {
    std::string key;
    std::string reason;
    double value = 0.0;
    bool present = false;
};

class MedicalConfiguration {
public:
    ~MedicalConfiguration();

    /// Load "key = value" text from `path`.
    ///
    /// With `verifyIntegrity` (the default), a sidecar "<path>.sha256" must
    /// exist and match, otherwise IntegrityError. A missing sidecar on a store
    /// that has never been committed is reported as IntegrityError too: an
    /// unverifiable calibration table is not a usable one.
    static Result<std::unique_ptr<MedicalConfiguration>> load(const std::string& path,
                                                              bool verifyIntegrity = true);

    /// In-memory store for defaults, tests and profiles that ship no file.
    static std::unique_ptr<MedicalConfiguration> empty(const std::string& path = {});

    Result<std::string> getString(const std::string& key) const;
    Result<double> getDouble(const std::string& key) const;
    Result<long long> getInt(const std::string& key) const;
    Result<bool> getBool(const std::string& key) const;

    std::string getString(const std::string& key, const std::string& fallback) const;
    double getDouble(const std::string& key, double fallback) const;
    long long getInt(const std::string& key, long long fallback) const;
    bool getBool(const std::string& key, bool fallback) const;

    /// Stage a change in memory. Nothing reaches storage until commit().
    Status set(const std::string& key, const std::string& value);
    Status setDouble(const std::string& key, double value);
    Status setInt(const std::string& key, long long value);

    bool has(const std::string& key) const;
    std::vector<std::string> keys() const;
    bool dirty() const noexcept;

    /// Atomically persist the store and rewrite the integrity sidecar.
    Status commit();

    /// Hex SHA-256 of the serialised store as it currently stands in memory.
    /// Belongs in the audit record of any configuration change.
    std::string digest() const;

    /// Check every declared limit. An empty vector means the configuration is
    /// safe to operate with; otherwise every violation is reported (the check
    /// does not stop at the first one, because an operator needs the whole
    /// picture before deciding).
    std::vector<SafetyViolation> validate(const std::vector<SafetyLimit>& limits) const;

    const std::string& path() const noexcept;

    MedicalConfiguration(const MedicalConfiguration&) = delete;
    MedicalConfiguration& operator=(const MedicalConfiguration&) = delete;

private:
    explicit MedicalConfiguration(std::string path);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace med
