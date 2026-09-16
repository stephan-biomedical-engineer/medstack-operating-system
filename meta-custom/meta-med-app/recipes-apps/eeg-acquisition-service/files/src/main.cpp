// SPDX-License-Identifier: MIT
//
// MedPlatform EEG acquisition service.
//
// The reference application of the MedApp layer, and the practical
// demonstration of the architecture's central claim: this file contains no
// system call, no sysfs path, no device tree knowledge and no conditional on
// the target hardware. Everything it does with the machine it does through
// MedFramework:
//
//   MedicalConfiguration - operating parameters and their safety limits
//   MedicalDevice        - the front-end: simulated, a co-processor over rpmsg,
//                          or a converter the kernel drives over industrial I/O
//   MedicalStorage       - session records on the encrypted volume
//   MedicalIPC           - the sample stream published to the HMI
//   MedicalLogger        - the audit trail
//   MedicalUpdate        - A/B slot confirmation after a successful start
//
// Porting the EEG to different silicon therefore changes zero lines here; it
// changes one value in a configuration file. That is the number the
// portability metric of the thesis reports.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <medplatform/MedicalConfiguration.h>
#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>
#include <medplatform/MedicalLogger.h>
#include <medplatform/MedicalStorage.h>
#include <medplatform/MedicalTypes.h>
#include <medplatform/MedicalUpdate.h>

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;

void handleSignal(int) {
    g_stopRequested = 1;
}

constexpr const char* kDefaultConfigPath = "/etc/medplatform/eeg.conf";
constexpr const char* kComponent = "eeg-acquisition-service";

/// The safety envelope the service refuses to start outside of. Deliberately
/// declared here, in the application, and not in the framework: what counts as
/// a safe sample rate is a property of the medical device, not of the platform.
///
/// These limits changed when the front-end became a real converter, and the
/// change is a clinical one rather than a hardware one - which is worth saying,
/// because the front-end plan's own test is that this file does not differ
/// between physical links. It still does not: the same table, the same binary
/// and the same source serve the simulated, co-processor and kernel-driven
/// links. What moved is the envelope itself, from ranges invented before there
/// was a converter to the values a 24-bit biopotential converter actually
/// offers.
std::vector<med::SafetyLimit> safetyLimits() {
    return {
        // A multiple of eight, up to four cascaded devices. The old range
        // [1, 64] admitted 12 electrodes, which is not a smaller montage - it
        // is a montage no hardware in this class can produce.
        {"acquisition.channels", 8.0, 32.0, true,
         "electrodes, in blocks of eight - one converter per block", {}, 8.0},

        // A set and not a range. The old lower bound of 125 Hz was
        // unreachable, and the old upper bound of 2000 Hz excluded three rates
        // the converter offers; between those two errors the range described
        // no device.
        {"acquisition.sample_rate_hz", 250.0, 16000.0, true,
         "output data rates the converter offers; below 250 Hz it has none",
         {250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0}, 0.0},

        {"acquisition.samples_per_frame", 1.0, 500.0, true, "acquisition block size",
         {}, 0.0},

        {"safety.max_input_uv", 10.0, 5000.0, true,
         "input range beyond which a sample is an artefact, not a signal", {}, 0.0},

        {"afe.gain", 1.0, 24.0, true,
         "programmable gain; with the reference it fixes full scale",
         {1.0, 2.0, 4.0, 6.0, 8.0, 12.0, 24.0}, 0.0},

        {"afe.reference_uv", 1000.0, 5000000.0, true,
         "converter reference voltage in microvolts", {}, 0.0},
    };
}

/// The check no table of independent limits can express: that the artefact
/// threshold is inside what the front-end can represent.
///
/// This is the DERS pattern taken one step further than a per-parameter bound.
/// safety.max_input_uv and afe.gain are individually reasonable at 500 uV and
/// 24, and individually reasonable at 5000 uV and 24, and the second pair is
/// not: full scale at gain 24 with a 4.5 V reference is 187500 uV, so a
/// threshold above that is a threshold the hardware can never report crossing.
/// A device whose safety limit is beyond its own measuring range does not have
/// that safety limit.
///
/// It lives here and not in the framework because it is a relation between two
/// named clinical parameters of one device. The framework supplies the shapes a
/// value may take; which values must relate to which is the device's own.
std::vector<med::SafetyViolation> relationalLimits(const med::MedicalConfiguration& config) {
    std::vector<med::SafetyViolation> violations;

    const double reference = config.getDouble("afe.reference_uv", 0.0);
    const double gain = config.getDouble("afe.gain", 0.0);
    const double threshold = config.getDouble("safety.max_input_uv", 0.0);
    if (reference <= 0.0 || gain <= 0.0) {
        return violations;  // already reported by the per-parameter limits
    }

    const double fullScale = reference / gain;
    if (threshold > fullScale) {
        med::SafetyViolation violation;
        violation.key = "safety.max_input_uv";
        violation.value = threshold;
        violation.present = true;
        violation.reason =
            "the artefact threshold is above the front-end's full scale of " +
            std::to_string(fullScale) +
            " uV, so a sample can never be seen to cross it";
        violations.push_back(violation);
    }

    return violations;
}

/// Hand the driver its options: every "device.option.<name>" key, with the
/// prefix removed.
///
/// Nothing here is conditional on the link. The afe.* prescription above is
/// translated into the active link's vocabulary when the image is built - see
/// do_derive_device_options in the recipe - so this binary passes the same
/// kind of line through on every target, and the driver on the other side
/// honours or refuses each one. A refusal stops the service, which is the
/// behaviour a setting nobody can apply has to have.
std::map<std::string, std::string> driverOptions(const med::MedicalConfiguration& config) {
    constexpr const char* kPrefix = "device.option.";
    const std::size_t prefixLength = std::strlen(kPrefix);

    std::map<std::string, std::string> options;
    for (const std::string& key : config.keys()) {
        if (key.rfind(kPrefix, 0) == 0 && key.size() > prefixLength) {
            options[key.substr(prefixLength)] = config.getString(key, "");
        }
    }
    return options;
}

/// Serialise a frame in the same wire format the AMP link uses, so the HMI
/// decodes what the co-processor produces and what the simulator produces with
/// exactly the same code path.
void encodeFrame(const med::SampleFrame& frame, double sampleRateHz,
                 std::vector<unsigned char>& out) {
    const std::size_t payloadBytes = frame.samples.size() * sizeof(std::int32_t);
    out.resize(sizeof(med::amp::FrameHeader) + payloadBytes);

    unsigned char* payload = out.data() + sizeof(med::amp::FrameHeader);
    for (std::size_t i = 0; i < frame.samples.size(); ++i) {
        // Nanovolt units: one LSB is 1 nV, so scaleNanoUnitsPerLsb is 1 and a
        // float microvolt value survives the round trip to within 1 nV.
        const std::int32_t raw =
            static_cast<std::int32_t>(frame.samples[i] * 1000.0F);
        std::memcpy(payload + i * sizeof(std::int32_t), &raw, sizeof(raw));
    }

    med::amp::FrameHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = med::amp::kFrameMagic;
    header.version = med::amp::kFrameVersion;
    header.channelCount = frame.channelCount;
    header.samplesPerChannel = frame.samplesPerChannel;
    header.sampleRateMilliHz = static_cast<std::uint32_t>(sampleRateHz * 1000.0);
    header.sequence = frame.sequence;
    header.timestampMicros = med::toUnixMicros(frame.timestamp);
    header.scaleNanoUnitsPerLsb = 1;
    header.crc32 = med::amp::crc32(payload, payloadBytes);

    std::memcpy(out.data(), &header, sizeof(header));
}

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
    loggerConfig.chainStatePath = "/var/lib/medplatform/eeg-audit-chain.state";
    logger.configure(loggerConfig);

    logger.audit(med::AuditEvent::SystemStart, "EEG acquisition service starting",
                 {{"config", configPath}, {"framework", med::frameworkVersion()}});

    // ---------------------------------------------------------------- config
    med::Result<std::unique_ptr<med::MedicalConfiguration>> configuration =
        med::MedicalConfiguration::load(configPath, true);
    if (!configuration) {
        // An unverifiable calibration table is a stop condition, not a warning:
        // every sample the service would produce is scaled by these numbers.
        logger.audit(med::AuditEvent::SelfTestFailed,
                     "configuration could not be loaded or verified",
                     {{"config", configPath},
                      {"status", med::toString(configuration.status())},
                      {"detail", configuration.message()}});
        std::fprintf(stderr, "eeg-acquisition-service: %s\n", configuration.message().c_str());
        return 1;
    }
    med::MedicalConfiguration& config = *configuration.value();

    std::vector<med::SafetyViolation> violations = config.validate(safetyLimits());
    {
        const std::vector<med::SafetyViolation> relational = relationalLimits(config);
        violations.insert(violations.end(), relational.begin(), relational.end());
    }
    if (!violations.empty()) {
        for (const med::SafetyViolation& violation : violations) {
            logger.audit(med::AuditEvent::SafetyLimitViolation, violation.reason,
                         {{"parameter", violation.key},
                          {"value", std::to_string(violation.value)},
                          {"present", violation.present ? "true" : "false"}});
            std::fprintf(stderr, "eeg-acquisition-service: unsafe parameter %s: %s\n",
                         violation.key.c_str(), violation.reason.c_str());
        }
        return 1;
    }

    logger.info("configuration verified",
                {{"digest", config.digest()}, {"config", configPath}});

    // ---------------------------------------------------------------- device
    med::DeviceConfig deviceConfig;
    deviceConfig.driver = config.getString("device.driver", "simulated");
    deviceConfig.id = config.getString("device.id", "eeg0");
    deviceConfig.address = config.getString("device.address", "");
    deviceConfig.channelCount =
        static_cast<std::uint16_t>(config.getInt("acquisition.channels", 8));
    deviceConfig.sampleRateHz = config.getDouble("acquisition.sample_rate_hz", 250.0);
    deviceConfig.samplesPerFrame =
        static_cast<std::uint32_t>(config.getInt("acquisition.samples_per_frame", 25));
    deviceConfig.driverOptions = driverOptions(config);

    med::Result<std::unique_ptr<med::MedicalDevice>> deviceResult =
        med::MedicalDeviceFactory::create(deviceConfig);
    if (!deviceResult) {
        logger.audit(med::AuditEvent::DeviceFault, "no usable acquisition front-end",
                     {{"driver", deviceConfig.driver},
                      {"detail", deviceResult.message()}});
        std::fprintf(stderr, "eeg-acquisition-service: %s\n", deviceResult.message().c_str());
        return 1;
    }
    med::MedicalDevice& device = *deviceResult.value();

    const med::Status selfTest = device.selfTest();
    if (selfTest != med::Status::Ok) {
        logger.audit(med::AuditEvent::SelfTestFailed, "front-end self test failed",
                     {{"driver", deviceConfig.driver},
                      {"status", med::toString(selfTest)}});
        return 1;
    }
    logger.audit(med::AuditEvent::SelfTestPassed, "front-end self test passed",
                 {{"driver", device.info().driver},
                  {"model", device.info().model},
                  {"channels", std::to_string(device.info().channelCount)},
                  {"sample rate hz", std::to_string(device.info().sampleRateHz)}});

    // --------------------------------------------------------------- storage
    med::StorageConfig storageConfig;
    storageConfig.root = config.getString("storage.root", "/data");
    storageConfig.nameSpace = config.getString("storage.namespace", "eeg");
    storageConfig.requireEncryptedBacking = config.getBool("storage.require_encryption", true);

    if (!storageConfig.requireEncryptedBacking) {
        // Running without an encrypted record store is a legitimate
        // development configuration and an illegitimate clinical one. Either
        // way it is a decision, so it goes in the audit trail.
        logger.audit(med::AuditEvent::SecurityEvent,
                     "record encryption requirement disabled by configuration",
                     {{"storage root", storageConfig.root}});
    }

    med::Result<std::unique_ptr<med::MedicalStorage>> storageResult =
        med::MedicalStorage::open(storageConfig);
    if (!storageResult) {
        logger.audit(med::AuditEvent::DeviceFault, "record storage is unusable",
                     {{"status", med::toString(storageResult.status())},
                      {"detail", storageResult.message()}});
        std::fprintf(stderr, "eeg-acquisition-service: %s\n", storageResult.message().c_str());
        return 1;
    }
    med::MedicalStorage& storage = *storageResult.value();

    // ------------------------------------------------------------------- ipc
    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = config.getString("ipc.socket", "/run/medplatform/eeg.sock");
    endpoint.socketMode = 0660;

    med::Result<std::unique_ptr<med::MedicalIpcServer>> serverResult =
        med::MedicalIpcServer::listen(endpoint);
    if (!serverResult) {
        logger.audit(med::AuditEvent::DeviceFault, "cannot publish the sample stream",
                     {{"socket", endpoint.address}, {"detail", serverResult.message()}});
        std::fprintf(stderr, "eeg-acquisition-service: %s\n", serverResult.message().c_str());
        return 1;
    }
    med::MedicalIpcServer& server = *serverResult.value();

    // ---------------------------------------------------- A/B slot confirmation
    // Reaching this point means the software that was installed into this slot
    // came up, passed its self test and opened every resource it needs. That -
    // not "the kernel booted" - is the condition under which an update may be
    // confirmed. Until it is, the bootloader still falls back to the previous
    // slot.
    med::Result<std::unique_ptr<med::MedicalUpdate>> updateResult =
        med::MedicalUpdate::connect();
    if (updateResult) {
        med::MedicalUpdate& update = *updateResult.value();
        med::Result<std::string> slot = update.bootSlot();
        med::Result<std::string> marked = update.markBootedGood();
        logger.audit(med::AuditEvent::UpdateSucceeded, "booted slot confirmed good",
                     {{"slot", slot ? slot.value() : "unknown"},
                      {"marked", marked ? marked.value() : "none"}});
    } else {
        logger.info("no update service available, skipping slot confirmation",
                    {{"detail", updateResult.message()}});
    }

    // --------------------------------------------------------------- session
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);  // a departing HMI must not kill acquisition

    const std::string sessionId = med::formatTimestamp(med::Clock::now());
    const std::string sessionDir = "session-" + sessionId;
    const med::DeviceInfo info = device.info();
    const std::string link = config.getString("acquisition.link", "unknown");

    {
        std::string metadata;
        metadata += "{\n";
        metadata += "  \"session\": \"" + sessionId + "\",\n";
        metadata += "  \"device_id\": \"" + info.id + "\",\n";
        metadata += "  \"driver\": \"" + info.driver + "\",\n";
        metadata += "  \"model\": \"" + info.model + "\",\n";
        // The physical link, in the record itself and not in a footnote.
        //
        // The three links do not know a sample's time equally well: on "amp"
        // the timestamp is taken in the data-ready interrupt on the real-time
        // core, on "spi" one SPI transfer later, and on "usb" after a bus frame
        // and a scheduling delay - an error of phase, not of date, and one that
        // no arithmetic downstream can undo. A reader comparing two recordings
        // has to be able to see which of those produced each. "simulated" says
        // the loudest thing of all: nothing was measured.
        metadata += "  \"link\": \"" + link + "\",\n";
        metadata += "  \"channels\": " + std::to_string(info.channelCount) + ",\n";
        metadata += "  \"sample_rate_hz\": " + std::to_string(info.sampleRateHz) + ",\n";
        metadata += "  \"unit\": \"" + info.unit + "\",\n";
        metadata += "  \"config_digest\": \"" + config.digest() + "\",\n";
        metadata += "  \"software_version\": \"" + std::string(med::frameworkVersion()) + "\"\n";
        metadata += "}\n";
        storage.writeString(sessionDir + "/metadata.json", metadata);
    }

    if (device.start() != med::Status::Ok) {
        logger.audit(med::AuditEvent::DeviceFault, "front-end refused to start", {});
        return 1;
    }
    logger.audit(med::AuditEvent::AcquisitionStarted, "acquisition session started",
                 {{"session", sessionId}, {"driver", info.driver}, {"link", link}});

    std::vector<std::unique_ptr<med::MedicalIpcChannel>> clients;
    std::vector<unsigned char> encoded;
    std::uint64_t frames = 0;
    std::uint64_t dropped = 0;
    /// Frames a viewer could not keep up with. Not the same failure as
    /// `dropped`, which counts frames that never reached the record: a viewer
    /// missing a frame costs nothing clinically, and the only reason to count
    /// it at all is that the alternative - waiting for the viewer - costs
    /// samples.
    std::uint64_t viewerMissed = 0;

    while (g_stopRequested == 0) {
        // Non-blocking: a viewer connecting or leaving must never stall the
        // acquisition loop.
        med::Result<std::unique_ptr<med::MedicalIpcChannel>> client =
            server.accept(std::chrono::milliseconds(0));
        if (client) {
            logger.info("HMI client connected", {{"clients", std::to_string(clients.size() + 1)}});
            clients.push_back(client.take());
        }

        med::Result<med::SampleFrame> frame = device.read(std::chrono::milliseconds(500));
        if (!frame) {
            if (frame.status() == med::Status::Timeout) {
                continue;
            }
            logger.audit(med::AuditEvent::DeviceFault, "acquisition read failed",
                         {{"status", med::toString(frame.status())},
                          {"detail", frame.message()}});
            break;
        }
        ++frames;

        encodeFrame(frame.value(), info.sampleRateHz, encoded);

        // The record is written before it is published: what a clinician later
        // reviews is authoritative, what the screen shows is a convenience.
        const med::Status stored =
            storage.append(sessionDir + "/raw.bin", encoded.data(), encoded.size());
        if (stored != med::Status::Ok) {
            ++dropped;
            if (dropped == 1 || dropped % 100 == 0) {
                logger.error("sample frame could not be persisted",
                             {{"status", med::toString(stored)},
                              {"dropped", std::to_string(dropped)}});
            }
        }

        for (std::size_t i = 0; i < clients.size();) {
            const med::Status published =
                clients[i]->send(encoded.data(), encoded.size());

            if (published == med::Status::WouldBlock) {
                // The viewer is present and behind. It loses this frame and
                // acquisition keeps its own clock - the opposite of the
                // previous behaviour, where a blocking write let the display
                // set the sample rate and the record silently contained fewer
                // samples than its own metadata claimed.
                ++viewerMissed;
                if (viewerMissed == 1 || viewerMissed % 250 == 0) {
                    logger.info("HMI client is not keeping up with the sample stream",
                                {{"missed frames", std::to_string(viewerMissed)},
                                 {"clients", std::to_string(clients.size())}});
                }
                ++i;
                continue;
            }

            if (published != med::Status::Ok) {
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                logger.info("HMI client disconnected",
                            {{"clients", std::to_string(clients.size())},
                             {"status", med::toString(published)}});
                continue;
            }
            ++i;
        }
    }

    device.stop();
    logger.audit(med::AuditEvent::AcquisitionStopped, "acquisition session stopped",
                 {{"session", sessionId},
                  {"frames", std::to_string(frames)},
                  {"dropped", std::to_string(dropped)},
                  // In the audit record and not only in a counter: a viewer
                  // that missed frames is a statement about what the operator
                  // saw, which is not the same as what was recorded, and a
                  // reviewer of the session is entitled to know the two
                  // differed.
                  {"viewer missed", std::to_string(viewerMissed)}});
    logger.audit(med::AuditEvent::PatientDataWritten, "session record closed",
                 {{"session", sessionId}, {"path", storage.basePath() + "/" + sessionDir}});
    logger.audit(med::AuditEvent::SystemStop, "EEG acquisition service stopped", {});

    return 0;
}
