// SPDX-License-Identifier: MIT
//
// eeg-record-export - derives the clinical file of every finished EEG session.
//
// Runs as a oneshot, after the acquisition service stops and once at boot,
// never in acquisition's path (implementation_plan_bdf.md, D4). Idempotent: a
// session with an export marker has been dealt with, converted or refused, and
// is left alone. A session that is being written is never touched (D5); one
// that was never closed was interrupted, and its clinical file says so.
//
// Like the service beside it, it does what it does through MedFramework and
// nothing else.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <medplatform/MedicalConfiguration.h>
#include <medplatform/MedicalLogger.h>
#include <medplatform/MedicalStorage.h>
#include <medplatform/MedicalTypes.h>

#include "record_export.h"

namespace {

constexpr const char* kDefaultConfigPath = "/etc/medplatform/eeg.conf";
constexpr const char* kComponent = "eeg-record-export";

std::string readMachineId() {
    std::FILE* file = std::fopen("/etc/machine-id", "re");
    if (file == nullptr) {
        return "unknown";
    }
    char buffer[64] = {0};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    std::string id(buffer, read);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) {
        id.pop_back();
    }
    return id.empty() ? "unknown" : id;
}

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    std::string configPath = kDefaultConfigPath;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if ((argument == "--config" || argument == "-c") && i + 1 < argc) {
            configPath = argv[++i];
        } else if (argument == "--help" || argument == "-h") {
            std::printf("usage: %s [--config %s]\n", argv[0], kDefaultConfigPath);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
            return 2;
        }
    }

    med::MedicalLogger& logger = med::MedicalLogger::instance();
    med::LoggerConfig loggerConfig;
    loggerConfig.component = kComponent;
    loggerConfig.deviceId = readMachineId();
    loggerConfig.softwareVersion = med::frameworkVersion();
    loggerConfig.chainStatePath = "/var/lib/medplatform/eeg-export-audit-chain.state";
    logger.configure(loggerConfig);

    med::Result<std::unique_ptr<med::MedicalConfiguration>> configuration =
        med::MedicalConfiguration::load(configPath, true);
    if (!configuration) {
        logger.error("configuration could not be loaded or verified",
                     {{"config", configPath}, {"detail", configuration.message()}});
        std::fprintf(stderr, "eeg-record-export: %s\n", configuration.message().c_str());
        return 1;
    }
    med::MedicalConfiguration& config = *configuration.value();

    med::StorageConfig storageConfig;
    storageConfig.root = config.getString("storage.root", "/data");
    storageConfig.nameSpace = config.getString("storage.namespace", "eeg");
    storageConfig.requireEncryptedBacking = config.getBool("storage.require_encryption", true);
    med::Result<std::unique_ptr<med::MedicalStorage>> opened =
        med::MedicalStorage::open(storageConfig);
    if (!opened) {
        logger.error("record storage is unusable", {{"detail", opened.message()}});
        std::fprintf(stderr, "eeg-record-export: %s\n", opened.message().c_str());
        return 1;
    }
    med::MedicalStorage& storage = *opened.value();

    // The sessions first, the live marker second. The service names a
    // session live before creating its directory, so in this order a
    // directory listed here is either named live by the marker read next or
    // was not live when listed. In the other order a session opened between
    // the two reads would look interrupted, and be converted under its writer.
    med::Result<std::vector<std::string>> entries = storage.list();
    if (!entries) {
        logger.error("cannot list the sessions", {{"detail", entries.message()}});
        return 1;
    }

    std::string live;
    {
        med::StorageConfig runtimeConfig;
        runtimeConfig.root = config.getString("storage.runtime", "/run/medplatform");
        runtimeConfig.nameSpace = "";
        runtimeConfig.requireEncryptedBacking = false;
        med::Result<std::unique_ptr<med::MedicalStorage>> runtime =
            med::MedicalStorage::open(runtimeConfig);
        // No runtime directory: the service is not running, nothing is live.
        if (runtime && runtime.value()->exists("session")) {
            med::Result<std::string> named = runtime.value()->readString("session");
            if (!named) {
                // Unreadable is not "nobody": converting now could convert
                // the session being written. The next run will see.
                logger.error("the live session marker exists and cannot be read; "
                             "nothing converted",
                             {{"detail", named.message()}});
                return 1;
            }
            live = trimmed(named.value());
        }
    }

    int failures = 0;
    for (const std::string& entry : entries.value()) {
        if (entry.rfind("session-", 0) != 0) {
            continue;
        }
        const std::string id = entry.substr(8);
        if (id == live || storage.exists(entry + "/export.json")) {
            continue;
        }
        const bool interrupted = !storage.exists(entry + "/closed.json");

        med::Result<eeg::ExportSummary> exported = eeg::exportSession(storage, entry, interrupted);
        if (!exported) {
            const bool refusal = exported.status() == med::Status::IntegrityError ||
                                 exported.status() == med::Status::InvalidArgument;
            if (!refusal) {
                // An I/O failure says nothing about the session: no marker,
                // so the next run tries again.
                logger.error("clinical file could not be written",
                             {{"session", id},
                              {"status", med::toString(exported.status())},
                              {"detail", exported.message()}});
                ++failures;
                continue;
            }
            // A refusal is a statement about the record, and it is final:
            // marked, audited, not retried at every boot.
            storage.writeString(entry + "/export.json", eeg::refusalMarker(exported.message()));
            logger.audit(med::AuditEvent::SecurityEvent, "clinical file refused",
                         {{"session", id},
                          {"status", med::toString(exported.status())},
                          {"reason", exported.message()}});
            continue;
        }

        const eeg::ExportSummary& summary = exported.value();
        for (const eeg::ExportedFile& file : summary.files) {
            logger.audit(med::AuditEvent::PatientDataWritten, "clinical file written",
                         {{"session", id},
                          {"file", file.name},
                          {"sha256", file.sha256},
                          {"records", std::to_string(file.records)}});
        }
        const med::Status marked =
            storage.writeString(entry + "/export.json", eeg::exportMarker(summary));
        if (marked != med::Status::Ok) {
            logger.error("export marker could not be written",
                         {{"session", id}, {"status", med::toString(marked)}});
            ++failures;
            continue;
        }
        logger.info("session converted",
                    {{"session", id},
                     {"files", std::to_string(summary.files.size())},
                     {"frames", std::to_string(summary.frames)},
                     {"missing", std::to_string(summary.missing)},
                     {"failed crc", std::to_string(summary.failedCrc)},
                     {"interrupted", summary.interrupted ? "true" : "false"}});
    }

    // --- hand the finished clinical files to the reviewer -------------------
    // Every closed, converted session - not only the ones converted in this
    // run - so that a file whose publication failed once, or that was derived
    // before this device had a reviewer, is published the next time. Never the
    // live session: it has no clinical file, and its directory is not listed
    // here at all.
    //
    // Only the clinical file gets a second name. The raw record, the metadata
    // and the markers beside it stay where they are, readable by this identity
    // and no other.
    const std::string reviewGroup = trimmed(config.getString("storage.review_group", ""));
    if (!reviewGroup.empty()) {
        std::uint64_t published = 0;
        bool noSuchGroup = false;
        for (const std::string& entry : entries.value()) {
            if (noSuchGroup || entry.rfind("session-", 0) != 0) {
                continue;
            }
            const std::string id = entry.substr(8);
            if (id == live || !storage.exists(entry + "/export.json")) {
                continue;
            }
            med::Result<std::vector<std::string>> files = storage.list(entry);
            if (!files) {
                continue;
            }
            for (const std::string& file : files.value()) {
                const std::string suffix = ".bdf";
                if (file.rfind("record", 0) != 0 || file.size() <= suffix.size() ||
                    file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0) {
                    continue;
                }
                // "record.bdf" of session X is X.bdf; "record-002.bdf" is
                // X-002.bdf. One flat directory: the reviewer's file dialog
                // shows sessions, not this program's layout.
                const std::string part = file.substr(6, file.size() - 6 - suffix.size());
                const std::string name = id + part + suffix;
                const med::Status status = storage.publish(entry + "/" + file, name, reviewGroup);
                if (status == med::Status::Unavailable) {
                    // An image that ships no reviewer has no such group. That
                    // is a configuration, not a fault - said once, not per file.
                    logger.info("no reviewing identity on this system; clinical files stay private",
                                {{"group", reviewGroup}});
                    noSuchGroup = true;
                    break;
                }
                if (status != med::Status::Ok) {
                    logger.error("clinical file could not be published for review",
                                 {{"session", id},
                                  {"file", file},
                                  {"status", med::toString(status)}});
                    ++failures;
                    continue;
                }
                ++published;
            }
        }
        if (!noSuchGroup) {
            logger.info("clinical files available for review",
                        {{"group", reviewGroup}, {"files", std::to_string(published)}});
        }
    }

    return failures == 0 ? 0 : 1;
}
