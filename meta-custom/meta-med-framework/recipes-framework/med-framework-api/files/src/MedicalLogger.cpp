// SPDX-License-Identifier: MIT

#include "MedicalLogger.h"

#include <systemd/sd-journal.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "MedDigest.h"

namespace med {
namespace {

/// journald field names accept [A-Z0-9_] and must not start with a digit.
/// Everything else in a caller supplied key is folded to '_'.
std::string normaliseFieldName(const std::string& key) {
    std::string out = "MED_";
    for (const char c : key) {
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out.push_back(c);
        } else if (c >= 'a' && c <= 'z') {
            out.push_back(static_cast<char>(c - 'a' + 'A'));
        } else {
            out.push_back('_');
        }
    }
    return out;
}

/// The serialised form a record's digest is computed over. Field order is the
/// caller's order, deliberately: a verifier replaying the journal reads the
/// fields back in the order journald stored them.
std::string serialiseRecord(std::uint64_t sequence, const std::string& timestamp,
                            const char* event, const std::string& message,
                            const std::vector<LogField>& fields) {
    std::string record;
    record.reserve(128 + message.size());
    record.append(std::to_string(sequence)).push_back('|');
    record.append(timestamp).push_back('|');
    record.append(event).push_back('|');
    record.append(message);
    for (const LogField& field : fields) {
        record.push_back('|');
        record.append(field.key).push_back('=');
        record.append(field.value);
    }
    return record;
}

Status writeFileAtomically(const std::string& path, const std::string& content) {
    const std::string temporary = path + ".tmp";

    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return Status::IoError;
    }

    Status status = Status::Ok;
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = Status::IoError;
            break;
        }
        written += static_cast<std::size_t>(n);
    }

    if (status == Status::Ok && ::fsync(fd) != 0) {
        status = Status::IoError;
    }
    ::close(fd);

    if (status != Status::Ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return Status::IoError;
    }
    return Status::Ok;
}

void ensureParentDirectory(const std::string& path) {
    const std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return;
    }
    // One level is enough for the paths the framework uses
    // (/var/lib/medplatform/...); deeper trees are the integrator's job.
    ::mkdir(path.substr(0, slash).c_str(), 0700);
}

}  // namespace

const char* toString(Severity severity) noexcept {
    switch (severity) {
        case Severity::Emergency: return "EMERGENCY";
        case Severity::Alert:     return "ALERT";
        case Severity::Critical:  return "CRITICAL";
        case Severity::Error:     return "ERROR";
        case Severity::Warning:   return "WARNING";
        case Severity::Notice:    return "NOTICE";
        case Severity::Info:      return "INFO";
        case Severity::Debug:     return "DEBUG";
    }
    return "UNKNOWN";
}

const char* toString(AuditEvent event) noexcept {
    switch (event) {
        case AuditEvent::SystemStart:          return "SYSTEM_START";
        case AuditEvent::SystemStop:           return "SYSTEM_STOP";
        case AuditEvent::SelfTestPassed:       return "SELF_TEST_PASSED";
        case AuditEvent::SelfTestFailed:       return "SELF_TEST_FAILED";
        case AuditEvent::ConfigurationChanged: return "CONFIGURATION_CHANGED";
        case AuditEvent::CalibrationChanged:   return "CALIBRATION_CHANGED";
        case AuditEvent::AcquisitionStarted:   return "ACQUISITION_STARTED";
        case AuditEvent::AcquisitionStopped:   return "ACQUISITION_STOPPED";
        case AuditEvent::PatientDataWritten:   return "PATIENT_DATA_WRITTEN";
        case AuditEvent::PatientDataRead:      return "PATIENT_DATA_READ";
        case AuditEvent::PatientDataExported:  return "PATIENT_DATA_EXPORTED";
        case AuditEvent::UpdateStarted:        return "UPDATE_STARTED";
        case AuditEvent::UpdateSucceeded:      return "UPDATE_SUCCEEDED";
        case AuditEvent::UpdateFailed:         return "UPDATE_FAILED";
        case AuditEvent::SafetyLimitViolation: return "SAFETY_LIMIT_VIOLATION";
        case AuditEvent::DeviceFault:          return "DEVICE_FAULT";
        case AuditEvent::SecurityEvent:        return "SECURITY_EVENT";
    }
    return "UNKNOWN";
}

struct MedicalLogger::Impl {
    mutable std::mutex mutex;
    LoggerConfig config;
    bool configured = false;

    /// Genesis value of the chain. All zeroes marks "no predecessor", so a
    /// verifier can tell a fresh chain from a truncated one.
    std::string chainDigest = std::string(64, '0');
    std::uint64_t sequence = 0;

    void loadChainState() {
        const int fd = ::open(config.chainStatePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return;  // first boot: keep the genesis chain
        }
        char buffer[128] = {0};
        const ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
        ::close(fd);
        if (n <= 0) {
            return;
        }

        unsigned long long storedSequence = 0;
        char storedDigest[80] = {0};
        if (std::sscanf(buffer, "%llu %79s", &storedSequence, storedDigest) == 2) {
            sequence = static_cast<std::uint64_t>(storedSequence);
            chainDigest = storedDigest;
        }
    }

    void persistChainState() {
        ensureParentDirectory(config.chainStatePath);
        const std::string content =
            std::to_string(sequence) + " " + chainDigest + "\n";
        // A failure here does not invalidate the record that was already
        // written to the journal; it only means the chain restarts from
        // genesis after a reboot, which a verifier sees as a chain break.
        (void)writeFileAtomically(config.chainStatePath, content);
    }
};

MedicalLogger::MedicalLogger() : impl_(new Impl()) {}
MedicalLogger::~MedicalLogger() = default;

MedicalLogger& MedicalLogger::instance() {
    // Function-local static: thread safe initialisation, and no dependency on
    // static initialisation order between translation units.
    static MedicalLogger logger;
    return logger;
}

Status MedicalLogger::configure(const LoggerConfig& config) {
    std::lock_guard<std::mutex> guard(impl_->mutex);

    const bool first = !impl_->configured;
    impl_->config = config;
    impl_->configured = true;

    if (first) {
        impl_->loadChainState();
    }
    return Status::Ok;
}

void MedicalLogger::log(Severity severity, const std::string& message,
                        const std::vector<LogField>& fields) {
    std::lock_guard<std::mutex> guard(impl_->mutex);

    std::vector<std::string> storage;
    storage.reserve(fields.size() + 6);
    storage.push_back("MESSAGE=" + message);
    storage.push_back("PRIORITY=" + std::to_string(static_cast<int>(severity)));
    storage.push_back("SYSLOG_IDENTIFIER=" + impl_->config.component);
    storage.push_back("MED_COMPONENT=" + impl_->config.component);
    if (!impl_->config.deviceId.empty()) {
        storage.push_back("MED_DEVICE_ID=" + impl_->config.deviceId);
    }
    if (!impl_->config.softwareVersion.empty()) {
        storage.push_back("MED_SW_VERSION=" + impl_->config.softwareVersion);
    }
    for (const LogField& field : fields) {
        storage.push_back(normaliseFieldName(field.key) + "=" + field.value);
    }

    std::vector<struct iovec> iov;
    iov.reserve(storage.size());
    for (std::string& entry : storage) {
        iov.push_back({const_cast<char*>(entry.data()), entry.size()});
    }
    sd_journal_sendv(iov.data(), static_cast<int>(iov.size()));

    if (impl_->config.alsoWriteStderr) {
        std::fprintf(stderr, "[%s] %s\n", toString(severity), message.c_str());
    }
}

void MedicalLogger::debug(const std::string& message, const std::vector<LogField>& fields) {
    log(Severity::Debug, message, fields);
}
void MedicalLogger::info(const std::string& message, const std::vector<LogField>& fields) {
    log(Severity::Info, message, fields);
}
void MedicalLogger::warning(const std::string& message, const std::vector<LogField>& fields) {
    log(Severity::Warning, message, fields);
}
void MedicalLogger::error(const std::string& message, const std::vector<LogField>& fields) {
    log(Severity::Error, message, fields);
}

Status MedicalLogger::audit(AuditEvent event, const std::string& message,
                            const std::vector<LogField>& fields) {
    std::lock_guard<std::mutex> guard(impl_->mutex);

    const std::string timestamp = formatTimestamp(Clock::now());
    const char* const eventName = toString(event);
    const std::uint64_t sequence = impl_->sequence + 1;

    const std::string record =
        serialiseRecord(sequence, timestamp, eventName, message, fields);
    const std::string digest = internal::sha256Hex(impl_->chainDigest + "|" + record);
    if (digest.empty()) {
        // No digest means no tamper evidence, and an audit record without
        // tamper evidence is worse than a loud failure.
        return Status::Internal;
    }

    std::vector<std::string> storage;
    storage.reserve(fields.size() + 12);
    storage.push_back("MESSAGE=" + message);
    // Audit records are Notice: they are expected, not problems, and must not
    // be filtered out by a severity threshold set for diagnostics.
    storage.push_back("PRIORITY=" + std::to_string(static_cast<int>(Severity::Notice)));
    storage.push_back("SYSLOG_IDENTIFIER=" + impl_->config.component);
    storage.push_back("MED_COMPONENT=" + impl_->config.component);
    storage.push_back("MED_AUDIT=1");
    storage.push_back(std::string("MED_AUDIT_EVENT=") + eventName);
    storage.push_back("MED_AUDIT_SEQ=" + std::to_string(sequence));
    storage.push_back("MED_AUDIT_TIME=" + timestamp);
    storage.push_back("MED_AUDIT_PREV=" + impl_->chainDigest);
    storage.push_back("MED_AUDIT_HASH=" + digest);
    if (!impl_->config.deviceId.empty()) {
        storage.push_back("MED_DEVICE_ID=" + impl_->config.deviceId);
    }
    if (!impl_->config.softwareVersion.empty()) {
        storage.push_back("MED_SW_VERSION=" + impl_->config.softwareVersion);
    }
    for (const LogField& field : fields) {
        storage.push_back(normaliseFieldName(field.key) + "=" + field.value);
    }

    std::vector<struct iovec> iov;
    iov.reserve(storage.size());
    for (std::string& entry : storage) {
        iov.push_back({const_cast<char*>(entry.data()), entry.size()});
    }

    if (sd_journal_sendv(iov.data(), static_cast<int>(iov.size())) < 0) {
        // The chain is only advanced once the record is in the journal.
        // Advancing it on a failed write would make the next record reference
        // a predecessor no verifier can find.
        return Status::IoError;
    }

    impl_->sequence = sequence;
    impl_->chainDigest = digest;
    impl_->persistChainState();

    if (impl_->config.alsoWriteStderr) {
        std::fprintf(stderr, "[AUDIT %s #%llu] %s\n", eventName,
                     static_cast<unsigned long long>(sequence), message.c_str());
    }
    return Status::Ok;
}

std::string MedicalLogger::chainDigest() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->chainDigest;
}

std::uint64_t MedicalLogger::auditSequence() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->sequence;
}

}  // namespace med
