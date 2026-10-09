// SPDX-License-Identifier: MIT
//
// MedFramework - structured, tamper-evident audit logging.
//
// Regulatory context (IEC 62304 5.1.1 / 9.x, FDA design history file): a
// medical device has to be able to answer, after the fact, what it did, when,
// with which software version and with which configuration. Two properties
// follow from that and are implemented here:
//
//   * Structured, not free text. Records go to systemd-journald as typed
//     fields (MED_AUDIT_EVENT, MED_DEVICE_ID, ...), so a reviewer queries the
//     trail instead of grepping it.
//   * Tamper evident. Every audit record carries the SHA-256 of
//     (previous digest || this record). Removing, reordering or editing a
//     record breaks the chain at that point and every point after it. The OS
//     complements this with journald Forward Secure Sealing (see
//     meta-med-distro 10-journald-audit.conf); the chain here survives a
//     journal rotation, the seal covers records this process never saw.
//
// The chain is an integrity witness, not a secret: it proves alteration, it
// does not prevent it.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "MedicalTypes.h"

namespace med {

/// syslog severities, kept numerically identical so they map straight onto
/// journald PRIORITY.
enum class Severity : int {
    Emergency = 0,
    Alert = 1,
    Critical = 2,
    Error = 3,
    Warning = 4,
    Notice = 5,
    Info = 6,
    Debug = 7,
};

/// The closed set of auditable events. Closed on purpose: an audit trail whose
/// vocabulary each application invents is not reviewable across a product
/// family.
enum class AuditEvent {
    SystemStart,
    SystemStop,
    SelfTestPassed,
    SelfTestFailed,
    ConfigurationChanged,
    CalibrationChanged,
    AcquisitionStarted,
    AcquisitionStopped,
    PatientDataWritten,
    PatientDataRead,
    PatientDataExported,
    UpdateStarted,
    UpdateSucceeded,
    UpdateFailed,
    SafetyLimitViolation,
    DeviceFault,
    SecurityEvent,
    /// A monitored electrode lost or regained contact with the subject.
    /// Appended, not inserted: the enumerators before it keep their values.
    ElectrodeContactChanged,
};

const char* toString(Severity severity) noexcept;
const char* toString(AuditEvent event) noexcept;

/// One structured journal field. `key` is normalised to upper case and
/// prefixed with MED_ before it reaches the journal.
struct LogField {
    std::string key;
    std::string value;
};

struct LoggerConfig {
    /// SYSLOG_IDENTIFIER and MED_COMPONENT of every record.
    std::string component = "medapp";
    /// Device serial / UDI. Attached to every record so a trail extracted from
    /// a fleet stays attributable.
    std::string deviceId;
    /// Software version under configuration control (IEC 62304 5.1.1).
    std::string softwareVersion;
    /// Where the hash chain head is persisted across restarts. Must be on
    /// writable, power-fail-safe storage.
    std::string chainStatePath = "/var/lib/medplatform/audit-chain.state";
    /// Mirror records to stderr. Development aid; journald already captures
    /// stderr of a systemd service, so this duplicates records when enabled.
    bool alsoWriteStderr = false;
};

/// Process-wide logger. A single instance keeps the hash chain monotonic
/// within a process; concurrent processes maintain independent chains and are
/// distinguished by MED_COMPONENT.
class MedicalLogger {
public:
    static MedicalLogger& instance();

    /// Idempotent; may be called again to update the software version after an
    /// update. Loads the persisted chain head on first call.
    Status configure(const LoggerConfig& config);

    void log(Severity severity, const std::string& message,
             const std::vector<LogField>& fields = {});

    void debug(const std::string& message, const std::vector<LogField>& fields = {});
    void info(const std::string& message, const std::vector<LogField>& fields = {});
    void warning(const std::string& message, const std::vector<LogField>& fields = {});
    void error(const std::string& message, const std::vector<LogField>& fields = {});

    /// Emit an audit record and extend the hash chain.
    /// Returns IoError if the record could not be handed to journald - the
    /// caller decides whether that is fatal for its use case (for a safety
    /// relevant event, it is).
    Status audit(AuditEvent event, const std::string& message,
                 const std::vector<LogField>& fields = {});

    /// Current chain head (hex SHA-256) and the sequence number of the last
    /// audit record. A verifier replays the journal and recomputes both.
    std::string chainDigest() const;
    std::uint64_t auditSequence() const;

    MedicalLogger(const MedicalLogger&) = delete;
    MedicalLogger& operator=(const MedicalLogger&) = delete;

private:
    MedicalLogger();
    ~MedicalLogger();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace med
