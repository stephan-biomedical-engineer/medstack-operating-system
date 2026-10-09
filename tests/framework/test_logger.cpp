// SPDX-License-Identifier: MIT
//
// MedicalLogger: the tamper-evident audit trail.
//
// Compiled only when libsystemd's headers are available - the logger writes
// through sd-journal, which is one of the framework's two declared
// dependencies. When it is absent, main() says so out loud rather than
// reporting a smaller suite as a green one.
//
// What is testable here is the HASH CHAIN, which is the part that carries the
// IEC 62304 traceability argument: each record's digest covers the previous
// one, so removing or editing a record breaks every digest after it. What is
// NOT testable here is whether journald actually stored the record and whether
// its own Forward Secure Sealing validates - that needs a running journald and
// a reader, and it is asserted on the target by make check instead.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MedicalLogger.h"
#include "check.h"
#include "temp_dir.h"

using namespace med;
using medtest::TempDir;

void testLogger() {
    medtest::section("MedicalLogger - vocabulário fechado");

    {
        // Severities map straight onto journald PRIORITY, so their numeric
        // values are an interface and not an ordering convenience.
        CHECK_EQ(static_cast<int>(Severity::Emergency), 0);
        CHECK_EQ(static_cast<int>(Severity::Error), 3);
        CHECK_EQ(static_cast<int>(Severity::Info), 6);
        CHECK_EQ(static_cast<int>(Severity::Debug), 7);

        // The audit vocabulary is closed on purpose: a trail whose events each
        // application invents is not reviewable across a product family. Every
        // one of them must have a distinct, stable spelling, because that
        // spelling is the journal field a verifier greps for.
        const AuditEvent events[] = {
            AuditEvent::SystemStart,          AuditEvent::SystemStop,
            AuditEvent::SelfTestPassed,       AuditEvent::SelfTestFailed,
            AuditEvent::ConfigurationChanged, AuditEvent::CalibrationChanged,
            AuditEvent::AcquisitionStarted,   AuditEvent::AcquisitionStopped,
            AuditEvent::PatientDataWritten,   AuditEvent::PatientDataRead,
            AuditEvent::PatientDataExported,  AuditEvent::UpdateStarted,
            AuditEvent::UpdateSucceeded,      AuditEvent::UpdateFailed,
            AuditEvent::SafetyLimitViolation, AuditEvent::DeviceFault,
            AuditEvent::SecurityEvent,        AuditEvent::ElectrodeContactChanged,
        };

        std::vector<std::string> names;
        for (const AuditEvent event : events) {
            const std::string name = toString(event);
            CHECK_MSG(!name.empty(), "um AuditEvent não tem grafia");
            names.push_back(name);
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            for (std::size_t j = i + 1; j < names.size(); ++j) {
                CHECK_MSG(names[i] != names[j],
                          "dois AuditEvent compartilham a grafia '" + names[i] + "'");
            }
        }
    }

    medtest::section("MedicalLogger - a cadeia de hash");

    {
        TempDir dir;
        CHECK(dir.valid());

        LoggerConfig config;
        config.component = "med-framework-tests";
        config.deviceId = "TEST-0000";
        config.softwareVersion = frameworkVersion();
        config.chainStatePath = dir.file("audit-chain.state");
        config.alsoWriteStderr = false;

        MedicalLogger& logger = MedicalLogger::instance();
        CHECK_STATUS(logger.configure(config), Status::Ok);

        // A fresh chain starts from a defined head, not from garbage: a
        // verifier replaying from the beginning has to know where "the
        // beginning" is.
        const std::string genesis = logger.chainDigest();
        CHECK_EQ(static_cast<long long>(genesis.size()), 64LL);
        CHECK_EQ(logger.auditSequence(), static_cast<std::uint64_t>(0));

        const Status first = logger.audit(AuditEvent::SystemStart, "teste de cadeia",
                                          {{"phase", "1"}});

        if (first != Status::Ok) {
            // No journald on this host (a container, typically). The chain is
            // deliberately NOT advanced when the record could not be handed
            // over, because advancing it would make the next record reference a
            // predecessor no verifier can find - so that behaviour is what gets
            // checked instead.
            CHECK_STATUS(first, Status::IoError);
            CHECK_EQ(logger.chainDigest(), genesis);
            CHECK_EQ(logger.auditSequence(), static_cast<std::uint64_t>(0));
            std::printf(
                "   AVISO  journald não aceitou o registro; a cadeia foi verificada\n"
                "          apenas no caminho de falha (que é o comportamento correto)\n");
            return;
        }

        const std::string afterFirst = logger.chainDigest();
        CHECK(afterFirst != genesis);
        CHECK_EQ(logger.auditSequence(), static_cast<std::uint64_t>(1));

        CHECK_STATUS(logger.audit(AuditEvent::AcquisitionStarted, "teste de cadeia",
                                  {{"phase", "2"}}),
                     Status::Ok);
        const std::string afterSecond = logger.chainDigest();
        CHECK(afterSecond != afterFirst);
        CHECK_EQ(logger.auditSequence(), static_cast<std::uint64_t>(2));

        // Two identical records must still produce different heads, otherwise
        // the chain could not distinguish "it happened twice" from "it happened
        // once and somebody duplicated the line".
        CHECK_STATUS(logger.audit(AuditEvent::AcquisitionStarted, "teste de cadeia",
                                  {{"phase", "2"}}),
                     Status::Ok);
        CHECK(logger.chainDigest() != afterSecond);
        CHECK_EQ(logger.auditSequence(), static_cast<std::uint64_t>(3));

        // The head survives a reconfigure, which is what "across restarts"
        // means for a process-wide singleton: the state file is re-read.
        const std::string beforeReconfigure = logger.chainDigest();
        const std::uint64_t sequenceBefore = logger.auditSequence();
        CHECK_STATUS(logger.configure(config), Status::Ok);
        CHECK_EQ(logger.chainDigest(), beforeReconfigure);
        CHECK_EQ(logger.auditSequence(), sequenceBefore);
    }
}
