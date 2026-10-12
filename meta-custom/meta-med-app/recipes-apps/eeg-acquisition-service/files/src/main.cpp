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
//   MedicalUpdate        - readiness, on which A/B slot confirmation waits
//
// Porting the EEG to different silicon therefore changes zero lines here; it
// changes one value in a configuration file. That is the number the
// portability metric of the thesis reports.

#include <csignal>
#include <cstdlib>
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

/// The electrodes in a pair of masks, 1-based as an operator counts channels:
/// "1+ 3-" is channel 1's positive input and channel 3's negative one.
std::string describeElectrodes(std::uint16_t positive, std::uint16_t negative,
                               bool referential) {
    std::string out;
    for (unsigned channel = 0; channel < 16; ++channel) {
        const unsigned bit = 1U << channel;
        if ((positive & bit) != 0) {
            out += (out.empty() ? "" : " ") + std::to_string(channel + 1) + "+";
        }
        if ((negative & bit) != 0 && !referential) {
            out += (out.empty() ? "" : " ") + std::to_string(channel + 1) + "-";
        }
    }
    // In a referential montage every inverting input is the one reference
    // electrode: whichever bit carries it, there is one thing to name, and
    // "1-" would point the operator at a channel instead of at the electrode
    // all of them depend on.
    if (referential && negative != 0) {
        out += (out.empty() ? "" : " ") + std::string("REF");
    }
    return out.empty() ? "none" : out;
}

/// A JSON string literal. Every value in the session metadata comes from a
/// sealed configuration, the framework or the clock, so none should need
/// escaping - and "should" is not a property a record format can rest on.
std::string jsonString(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

/// Whether `text` is a number JSON can carry as written. Stricter than the
/// configuration's own parser on purpose: strtod also takes "inf", "nan" and
/// hexadecimal, none of which is JSON.
bool isJsonNumber(const std::string& text) {
    if (text.empty() || !(text[0] == '-' || (text[0] >= '0' && text[0] <= '9'))) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' ||
              c == 'E')) {
            return false;
        }
    }
    char* end = nullptr;
    std::strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0';
}

/// A configuration value as JSON: a number where the sealed file has one, a
/// string otherwise, null where the key is absent - an absent declaration has
/// to read as absent, so that whatever consumes the metadata refuses rather
/// than assumes.
std::string jsonValue(const med::MedicalConfiguration& config, const std::string& key) {
    const med::Result<std::string> text = config.getString(key);
    if (!text) {
        return "null";
    }
    return isJsonNumber(text.value()) ? text.value() : jsonString(text.value());
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
                      {"status", med::toString(selfTest)},
                      {"detail", device.lastError()}});
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

    // Volatile state, through the same API and on the unit's RuntimeDirectory:
    // it is not a patient record and is not encrypted, so encryption is not
    // required of it. Required to exist, though. The program that derives the
    // clinical file treats a session with no close marker as interrupted
    // unless this directory names it as live, so a service that ran without
    // it would have its own live session converted, and marked interrupted,
    // underneath it.
    med::StorageConfig runtimeConfig;
    runtimeConfig.root = config.getString("storage.runtime", "/run/medplatform");
    runtimeConfig.nameSpace = "";
    runtimeConfig.requireEncryptedBacking = false;
    med::Result<std::unique_ptr<med::MedicalStorage>> runtimeResult =
        med::MedicalStorage::open(runtimeConfig);
    if (!runtimeResult) {
        logger.audit(med::AuditEvent::DeviceFault, "runtime state is unusable",
                     {{"status", med::toString(runtimeResult.status())},
                      {"detail", runtimeResult.message()}});
        std::fprintf(stderr, "eeg-acquisition-service: %s\n", runtimeResult.message().c_str());
        return 1;
    }
    med::MedicalStorage& runtime = *runtimeResult.value();

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

    // --------------------------------------------------------------- session
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);  // a departing HMI must not kill acquisition

    // Started before the session exists, not after. A session directory is a
    // claim that a recording was made; one created for a front-end that then
    // refused to start is a record of nothing, and a unit that restarts on
    // failure writes one per attempt - 54 of them in a minute on the board,
    // the first time the iio link met the service's sandbox. And the reason
    // goes into the audit record: the driver knows exactly which setting was
    // refused, and "refused to start" alone sent that diagnosis to a shell.
    {
        const med::Status started = device.start();
        if (started != med::Status::Ok) {
            logger.audit(med::AuditEvent::DeviceFault, "front-end refused to start",
                         {{"status", med::toString(started)},
                          {"detail", device.lastError()}});
            return 1;
        }
    }

    // ---------------------------------------------------- A/B slot confirmation
    // Reaching this point means the software that was installed into this slot
    // came up, passed its self test, opened every resource it needs and STARTED
    // its front-end. That - not "the kernel booted" - is the condition under
    // which an update may be confirmed, and this is where the service says so.
    //
    // It says so to the service manager, and nothing more. The unit is
    // Type=notify and required by boot-complete.target, and the OS marks the
    // booted slot good (rauc-mark-good.service) only once that target is
    // reached - so a slot whose acquisition never starts is never confirmed,
    // and the bootloader falls back when its attempts run out. This used to
    // call markBootedGood() itself, before device.start(), and both halves were
    // wrong: on the board, start() failed 54 times and the slot was confirmed
    // on every one, and rauc-mark-good.service had already confirmed it at
    // 12.9 s, before this service ran at all. One writer of the bootloader's
    // counters, and it is the OS.
    {
        const med::Status ready = med::MedicalUpdate::reportReady();
        logger.info("acquisition running, readiness reported to the service manager",
                    {{"status", med::toString(ready)}});
    }

    const med::DeviceInfo info = device.info();
    const std::string link = config.getString("acquisition.link", "unknown");
    // What the link configured (the recipe derives it; see record.montage in
    // the metadata), so that the audit names the electrode the bits mean.
    const bool referential = config.getString("record.montage", "") == "referential";

    // --------------------------------------------------------------- control
    // A second socket, apart from the sample stream: watching is not
    // commanding, and the two must be separable by file permission. One
    // request, one mandatory answer, in the same fixed-layout message the
    // framework already speaks to a producer of samples.
    med::IpcEndpoint controlEndpoint;
    controlEndpoint.transport = med::IpcTransport::UnixSeqpacket;
    controlEndpoint.address =
        config.getString("ipc.control_socket", "/run/medplatform/eeg-control.sock");
    controlEndpoint.socketMode = 0660;

    med::Result<std::unique_ptr<med::MedicalIpcServer>> controlResult =
        med::MedicalIpcServer::listen(controlEndpoint);
    if (!controlResult) {
        logger.audit(med::AuditEvent::DeviceFault, "cannot publish the control channel",
                     {{"socket", controlEndpoint.address}, {"detail", controlResult.message()}});
        std::fprintf(stderr, "eeg-acquisition-service: %s\n", controlResult.message().c_str());
        return 1;
    }
    med::MedicalIpcServer& control = *controlResult.value();

    // --------------------------------------------------------------- session
    // A session is not the life of this process. The process acquires and
    // publishes for as long as it runs, so that an operator can look at the
    // signal - and check the electrodes - before deciding to record. A session
    // is the part of that somebody asked to keep: it begins on a request, ends
    // on a request, and nothing is written to the record volume outside one.
    //
    // It used to be the life of the process, and every boot and every restart
    // of the unit made a record nobody had asked for: nine of them on the
    // board in one day of bench work, up to 82 MiB each.
    struct Session {
        bool open = false;
        std::string id;
        std::string dir;
        med::Timestamp start{};
        std::uint64_t frames = 0;
        std::uint64_t dropped = 0;
        /// Frames a viewer could not keep up with while this session was
        /// open. Not the same failure as `dropped`, which counts frames that
        /// never reached the record: a viewer missing a frame costs nothing
        /// clinically, and the only reason to count it at all is that the
        /// alternative - waiting for the viewer - costs samples.
        std::uint64_t viewerMissed = 0;
    };
    Session session;
    med::LeadOff lastContact;

    auto optional = [](const std::map<std::string, std::string>& request, const char* key) {
        const std::map<std::string, std::string>::const_iterator found = request.find(key);
        return found == request.end() || found->second.empty() ? std::string("null")
                                                               : jsonString(found->second);
    };

    // Opens a session. Empty on success, the reason otherwise - and on a
    // reason, nothing was left on the record volume.
    auto openSession = [&](const std::map<std::string, std::string>& request) -> std::string {
        session = Session{};
        session.id = med::formatTimestamp(med::Clock::now());
        session.dir = "session-" + session.id;
        session.start = med::Clock::now();

        // Named as live BEFORE its directory exists, so that there is no
        // instant at which the session is on disk, unclosed and unclaimed -
        // which is what an interrupted one looks like.
        const med::Status claimed = runtime.writeString("session", session.id + "\n");
        if (claimed != med::Status::Ok) {
            logger.audit(med::AuditEvent::DeviceFault, "cannot mark the session as live",
                         {{"session", session.id}, {"status", med::toString(claimed)}});
            return std::string("cannot mark the session as live");
        }

        std::string metadata;
        metadata += "{\n";
        metadata += "  \"session\": \"" + session.id + "\",\n";
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
        metadata += "  \"software_version\": \"" + std::string(med::frameworkVersion()) + "\",\n";

        // What the clinical file is derived from (implementation_plan_bdf.md,
        // D7): the scale the samples were quantised at. The program that
        // converts this session reads it here and nowhere else - not from an
        // eeg.conf that may have changed since - and refuses a session where
        // it is null.
        metadata += "  \"full_scale_uv\": " + jsonValue(config, "record.full_scale_uv") + ",\n";
        metadata += "  \"resolution_bits\": " + jsonValue(config, "record.resolution_bits") + ",\n";

        // The montage the link actually configured, and the names the
        // operator declared for the electrodes. null where nothing was
        // declared: generic labels, which is a statement about the record and
        // not a gap in it.
        metadata += "  \"montage\": " + jsonValue(config, "record.montage") + ",\n";
        metadata += "  \"prefiltering\": " + jsonValue(config, "record.prefiltering") + ",\n";
        {
            const std::string declared = config.getString("montage.electrodes", "");
            std::string list;
            std::size_t begin = declared.find_first_not_of(' ');
            while (begin != std::string::npos) {
                const std::size_t end = declared.find(' ', begin);
                list += (list.empty() ? "" : ", ") +
                        jsonString(declared.substr(begin, end - begin));
                begin = end == std::string::npos ? end : declared.find_first_not_of(' ', end);
            }
            metadata += "  \"electrodes\": " + (list.empty() ? std::string("null") : "[" + list + "]") +
                        ",\n";
        }
        metadata += "  \"reference\": " + jsonValue(config, "montage.reference") + ",\n";

        // The prescription the front-end was given, in the application's own
        // words. The config digest identifies it only for whoever still has
        // the file; this survives the file.
        {
            std::string prescription;
            for (const std::string& key : config.keys()) {
                if (key.rfind("afe.", 0) == 0) {
                    prescription += std::string(prescription.empty() ? "" : ",\n") + "    " +
                                    jsonString(key) + ": " + jsonValue(config, key);
                }
            }
            metadata += "  \"prescription\": {" +
                        (prescription.empty() ? std::string() : "\n" + prescription + "\n  ") +
                        "},\n";
        }

        // The date is only as good as the clock (implementation_plan_bdf.md,
        // D8). This board has no initialised RTC; the record says whether a
        // time source vouched for the clock, and which local time the
        // formats defined in local time will use.
        metadata += "  \"clock_synchronized\": " +
                    std::string(med::clockSynchronized() ? "true" : "false") + ",\n";
        metadata += "  \"utc_offset\": " + jsonString(med::utcOffset(session.start)) + ",\n";

        // Who asked for this session and under what name, AS THEY SAID IT.
        // Nothing here was verified: no person logged in, and the string came
        // over a socket from whoever was allowed to write to it. Named
        // "affirmed" in the record so that a reader does not take a claim for
        // an identity. null where the request named none.
        metadata += "  \"affirmed_operator\": " + optional(request, "operator") + ",\n";
        metadata += "  \"affirmed_exam\": " + optional(request, "exam") + ",\n";
        metadata += "  \"affirmed_subject\": " + optional(request, "subject") + "\n";
        metadata += "}\n";
        const med::Status written = storage.writeString(session.dir + "/metadata.json", metadata);
        if (written != med::Status::Ok) {
            runtime.remove("session");
            logger.audit(med::AuditEvent::DeviceFault, "session metadata could not be written",
                         {{"session", session.id}, {"status", med::toString(written)}});
            return std::string("the record volume refused the session");
        }

        session.open = true;
        // So that the first recorded frame states the electrodes' contact:
        // a session's audit trail begins with it, whatever was seen before.
        lastContact = med::LeadOff{};
        logger.audit(med::AuditEvent::AcquisitionStarted, "acquisition session started",
                     {{"session", session.id},
                      {"driver", info.driver},
                      {"link", link},
                      {"affirmed operator",
                       request.count("operator") != 0 ? request.at("operator") : ""},
                      {"affirmed exam", request.count("exam") != 0 ? request.at("exam") : ""}});
        return std::string();
    };

    auto closeSession = [&](const char* reason) {
        if (!session.open) {
            return;
        }
        // The close marker, in the session directory: the only thing on disk
        // that says the record ended because the service ended it, and how.
        // Written before the live marker is dropped, so that a session is
        // never unclosed and unclaimed at the same time. A session without it,
        // and not named as live, was interrupted - power loss, a killed
        // process - and the clinical file derived from it will say so.
        std::string closed;
        closed += "{\n";
        closed += "  \"session\": " + jsonString(session.id) + ",\n";
        closed += "  \"closed\": " + jsonString(med::formatTimestamp(med::Clock::now())) + ",\n";
        closed += "  \"reason\": " + jsonString(reason) + ",\n";
        closed += "  \"frames\": " + std::to_string(session.frames) + ",\n";
        closed += "  \"dropped\": " + std::to_string(session.dropped) + ",\n";
        closed += "  \"viewer_missed\": " + std::to_string(session.viewerMissed) + "\n";
        closed += "}\n";
        const med::Status written = storage.writeString(session.dir + "/closed.json", closed);
        if (written != med::Status::Ok) {
            logger.error("session close marker could not be written",
                         {{"session", session.id}, {"status", med::toString(written)}});
        }
        runtime.remove("session");
        // What the unit that derives the clinical file watches for. Its
        // content is not read: that program converts every closed session it
        // finds, and this only says that there is one more.
        runtime.writeString("last-closed", session.id + "\n");

        logger.audit(med::AuditEvent::AcquisitionStopped, "acquisition session stopped",
                     {{"session", session.id},
                      {"reason", reason},
                      {"frames", std::to_string(session.frames)},
                      {"dropped", std::to_string(session.dropped)},
                      // In the audit record and not only in a counter: a
                      // viewer that missed frames is a statement about what
                      // the operator saw, which is not the same as what was
                      // recorded, and a reviewer of the session is entitled to
                      // know the two differed.
                      {"viewer missed", std::to_string(session.viewerMissed)}});
        logger.audit(med::AuditEvent::PatientDataWritten, "session record closed",
                     {{"session", session.id}, {"path", storage.basePath() + "/" + session.dir}});
        session.open = false;
    };

    // One request in, one answer out. The answer is never optional and a
    // request that is not understood whole is refused whole: an option this
    // side does not know is not skipped, because the sender believes it was
    // honoured.
    auto answer = [&](const unsigned char* data, std::size_t length) -> std::vector<unsigned char> {
        const med::Result<std::map<std::string, std::string>> decoded =
            med::amp::decodeControl(data, length);
        if (!decoded) {
            logger.audit(med::AuditEvent::SecurityEvent, "control request refused",
                         {{"reason", decoded.message()}});
            return med::amp::encodeControlAck(1, decoded.message());
        }
        const std::map<std::string, std::string>& request = decoded.value();

        auto refuse = [&](const std::string& key, const std::string& reason) {
            // 1 + the position of the option at fault, in the order both
            // sides iterate; 1 when the fault is the request as a whole.
            std::uint16_t index = 1;
            std::uint16_t position = 0;
            for (const std::pair<const std::string, std::string>& entry : request) {
                if (entry.first == key) {
                    index = static_cast<std::uint16_t>(position + 1);
                }
                ++position;
            }
            logger.audit(med::AuditEvent::SecurityEvent, "control request refused",
                         {{"verb", request.count("verb") != 0 ? request.at("verb") : ""},
                          {"reason", reason}});
            return med::amp::encodeControlAck(index, reason);
        };

        const std::map<std::string, std::string>::const_iterator verb = request.find("verb");
        if (verb == request.end()) {
            return refuse("", "the request names no verb");
        }

        if (verb->second == "session.start") {
            for (const std::pair<const std::string, std::string>& entry : request) {
                if (entry.first != "verb" && entry.first != "operator" && entry.first != "exam" &&
                    entry.first != "subject") {
                    return refuse(entry.first, "unknown option: " + entry.first);
                }
            }
            if (session.open) {
                return refuse("verb", "a session is already open: " + session.id);
            }
            const std::string failed = openSession(request);
            if (!failed.empty()) {
                return refuse("verb", failed);
            }
            return med::amp::encodeControlAck(0, "recording " + session.id);
        }

        if (verb->second == "session.stop" || verb->second == "session.status") {
            if (request.size() != 1) {
                return refuse("", verb->second + " takes no option");
            }
            if (verb->second == "session.status") {
                // Not audited: a display asks this every second.
                return med::amp::encodeControlAck(
                    0, session.open ? "recording " + session.id + " " +
                                          std::to_string(session.frames)
                                    : std::string("idle"));
            }
            if (!session.open) {
                return refuse("verb", "no session is open");
            }
            const std::string id = session.id;
            const std::uint64_t recorded = session.frames;
            closeSession("stop requested");
            return med::amp::encodeControlAck(0, "closed " + id + " " + std::to_string(recorded));
        }

        return refuse("verb", "unknown verb: " + verb->second);
    };

    // For a bench routine that wants the old behaviour - a record for as long
    // as the unit runs - and says so in the configuration it runs under. The
    // shipped configuration says false, and a session then waits to be asked
    // for.
    if (config.getBool("session.autostart", false)) {
        const std::string failed = openSession({});
        if (!failed.empty()) {
            return 1;
        }
    }

    std::vector<std::unique_ptr<med::MedicalIpcChannel>> clients;
    std::vector<std::unique_ptr<med::MedicalIpcChannel>> controllers;
    std::vector<unsigned char> encoded;
    std::uint64_t viewerMissedTotal = 0;
    /// Why the loop ended. A front-end that stops answering is a fault, and the
    /// exit status has to say so: this used to fall through to `return 0`, and
    /// with Restart=on-failure systemd read a clean exit and did not restart.
    /// Unplugging the USB bridge for 10 s on the board ended acquisition for
    /// good - the bridge came back, passed its self test, and nothing ever
    /// read it again, with the unit "inactive" rather than "failed".
    bool deviceFault = false;

    while (g_stopRequested == 0) {
        // Non-blocking: a viewer connecting or leaving must never stall the
        // acquisition loop.
        med::Result<std::unique_ptr<med::MedicalIpcChannel>> client =
            server.accept(std::chrono::milliseconds(0));
        if (client) {
            logger.info("HMI client connected", {{"clients", std::to_string(clients.size() + 1)}});
            clients.push_back(client.take());
        }

        // Control, between two frames and never instead of one: the device
        // read below stays what paces this loop, so a request waits at most
        // one frame for its answer and costs acquisition nothing.
        med::Result<std::unique_ptr<med::MedicalIpcChannel>> controller =
            control.accept(std::chrono::milliseconds(0));
        if (controller) {
            controllers.push_back(controller.take());
        }
        for (std::size_t i = 0; i < controllers.size();) {
            unsigned char request[1024];
            const med::Result<std::size_t> received =
                controllers[i]->receive(request, sizeof(request), std::chrono::milliseconds(0));
            if (!received) {
                if (received.status() == med::Status::Timeout) {
                    ++i;
                    continue;
                }
                controllers.erase(controllers.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            const std::vector<unsigned char> reply = answer(request, received.value());
            if (controllers[i]->send(reply.data(), reply.size()) != med::Status::Ok) {
                controllers.erase(controllers.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }

        med::Result<med::SampleFrame> frame = device.read(std::chrono::milliseconds(500));
        if (!frame) {
            if (frame.status() == med::Status::Timeout) {
                continue;
            }
            logger.audit(med::AuditEvent::DeviceFault, "acquisition read failed",
                         {{"status", med::toString(frame.status())},
                          {"detail", frame.message()}});
            deviceFault = true;
            break;
        }

        med::amp::encodeFrame(frame.value(), info.sampleRateHz, encoded);

        if (session.open) {
            ++session.frames;

            // Electrode contact, audited when it changes and not per frame -
            // once per frame would bury the trail. The per-frame status needs
            // no line here: it is part of the encoded frame, so it is in the
            // record.
            const med::LeadOff& contact = frame.value().leadOff;
            if (session.frames == 1 ? contact.monitored() : contact != lastContact) {
                const bool detached =
                    contact.detachedPositive() != 0 || contact.detachedNegative() != 0;
                const char* what = !contact.monitored()   ? "electrode contact no longer monitored"
                                   : session.frames == 1 ? "electrode contact monitored"
                                   : detached            ? "electrode contact lost"
                                                         : "electrode contact restored";
                logger.audit(med::AuditEvent::ElectrodeContactChanged, what,
                             {{"session", session.id},
                              {"sequence", std::to_string(frame.value().sequence)},
                              {"monitored", describeElectrodes(contact.monitoredPositive,
                                                               contact.monitoredNegative,
                                                               referential)},
                              {"detached", describeElectrodes(contact.detachedPositive(),
                                                              contact.detachedNegative(),
                                                              referential)}});
            }
            lastContact = contact;

            // The record is written before it is published: what a clinician
            // later reviews is authoritative, what the screen shows is a
            // convenience.
            const med::Status stored =
                storage.append(session.dir + "/raw.bin", encoded.data(), encoded.size());
            if (stored != med::Status::Ok) {
                ++session.dropped;
                if (session.dropped == 1 || session.dropped % 100 == 0) {
                    logger.error("sample frame could not be persisted",
                                 {{"status", med::toString(stored)},
                                  {"dropped", std::to_string(session.dropped)}});
                }
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
                ++viewerMissedTotal;
                if (session.open) {
                    ++session.viewerMissed;
                }
                if (viewerMissedTotal == 1 || viewerMissedTotal % 250 == 0) {
                    logger.info("HMI client is not keeping up with the sample stream",
                                {{"missed frames", std::to_string(viewerMissedTotal)},
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

    // A session still open when the service goes is closed by the service,
    // and says why. It is a complete record of what was acquired until then.
    closeSession(deviceFault ? "front-end fault" : "service stopped");

    logger.audit(med::AuditEvent::SystemStop, "EEG acquisition service stopped", {});

    // Non-zero so that the unit's Restart=on-failure brings acquisition back
    // when the front-end does. While it is absent each attempt fails its self
    // test and leaves no session behind, so the retries cost a log line each
    // and nothing in the record.
    return deviceFault ? 1 : 0;
}
