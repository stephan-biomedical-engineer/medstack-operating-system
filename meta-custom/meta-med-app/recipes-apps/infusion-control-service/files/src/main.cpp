// SPDX-License-Identifier: MIT
//
// infusion-control-service - the controller (controller.h) on the framework's
// drivers: MedicalActuator and MedicalDevice to the mechanism, MedicalLogger
// for the trail, MedicalConfiguration for the sealed library, MedicalIPC for
// the operator's requests, MedicalUpdate to tell systemd the device is ready.
//
// Start-up, in order, and a failure at any step is a refusal to run:
//   1. the configuration loads, its seal verifies and every limit holds
//      (INF-03, INF-16);
//   2. the mechanism accepts the dead-man window and a first readback shows it
//      at rest and not tripped - the self test (INF-15);
//   3. a therapy the previous run left unfinished becomes an alarm, never a
//      resumed infusion (INF-11);
//   4. READY=1, so boot-complete.target - and the A/B slot - waits for a
//      controller that can drive its mechanism.
//
// Control requests use the framework's control codec, one message and one
// answer per request, "verb" naming the request:
//   program  drug syringe rate_ml_h vtbi_ml [confirm=1 operator]
//   start | pause | resume | stop | status
//   acknowledge operator | silence operator
// The full state is in the status file (control.status), key=value lines.

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <medplatform/MedicalActuator.h>
#include <medplatform/MedicalConfiguration.h>
#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>
#include <medplatform/MedicalLogger.h>
#include <medplatform/MedicalUpdate.h>

#include "controller.h"
#include "library.h"

namespace {

constexpr const char* kComponent = "infusion-control";
constexpr const char* kDefaultConfigPath = "/etc/medplatform/infusion.conf";

std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }

std::string readMachineId() {
    std::ifstream in("/etc/machine-id");
    std::string id;
    std::getline(in, id);
    return id;
}

/// Holds the OWNER of the actuator, not the actuator: an operator's
/// acknowledgement of a lost mechanism replaces it with a new session.
class ActuatorAdapter : public infusion::ActuatorPort {
public:
    explicit ActuatorAdapter(std::unique_ptr<med::MedicalActuator>& actuator) : actuator_(actuator) {}
    med::Status setRate(double mlPerHour) override {
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f", mlPerHour);
        return actuator_->command({{"rate_ml_h", text}});
    }
    med::Status heartbeat() override { return actuator_->heartbeat(); }
    med::Status safeState() override { return actuator_->safeState(); }
    std::string lastError() const override { return actuator_->lastError(); }

private:
    std::unique_ptr<med::MedicalActuator>& actuator_;
};

class AuditAdapter : public infusion::AuditPort {
public:
    void audit(med::AuditEvent event, const std::string& message,
               const std::vector<med::LogField>& fields) override {
        med::MedicalLogger::instance().audit(event, message, fields);
    }
};

infusion::Readback decode(const med::SampleFrame& frame) {
    infusion::Readback r;
    r.encoderMm = frame.at(0, 0) / 1e6;
    r.pressureMmHg = frame.at(1, 0) / 1e6;
    r.air = frame.at(2, 0) > 0.5e6;
    r.motor = frame.at(3, 0) > 0.5e6;
    r.tripped = frame.at(4, 0) > 0.5e6;
    return r;
}

bool writeAtomically(const std::string& path, const std::string& content) {
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if (fd < 0) {
        return false;
    }
    const bool written = ::write(fd, content.data(), content.size()) ==
                         static_cast<ssize_t>(content.size());
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!written || !synced || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

bool parseNumber(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    out = std::strtod(text.c_str(), &end);
    return errno == 0 && end != text.c_str() && *end == '\0' && std::isfinite(out);
}

struct Mechanism {
    std::unique_ptr<med::MedicalActuator> actuator;
    std::unique_ptr<med::MedicalDevice> readback;
};

/// Connect to the mechanism and wait for a readback at rest - the self test.
/// Used at start-up and again when an operator acknowledges a lost mechanism.
med::Status connectMechanism(const infusion::ServiceConfig& cfg, Mechanism& m,
                             infusion::Readback& first, std::string& why) {
    if (m.actuator) {
        m.actuator->stop();
    }
    if (m.readback) {
        m.readback->stop();
    }
    med::ActuatorConfig actuatorConfig;
    actuatorConfig.id = "mechanism";
    actuatorConfig.address = cfg.commandSocket;
    actuatorConfig.heartbeatPeriod = cfg.parameters.heartbeatPeriod;
    actuatorConfig.missedHeartbeats = cfg.missedHeartbeats;
    auto actuator = med::MedicalActuatorFactory::create(actuatorConfig);
    if (!actuator) {
        why = actuator.message();
        return actuator.status();
    }
    m.actuator = actuator.take();

    med::DeviceConfig deviceConfig;
    deviceConfig.driver = "seqpacket";
    deviceConfig.id = "mechanism-readback";
    deviceConfig.address = cfg.dataSocket;
    deviceConfig.channelCount = 5;
    deviceConfig.sampleRateHz = cfg.frameRateHz;
    deviceConfig.samplesPerFrame = 1;
    deviceConfig.channelUnits = {"mm", "mmHg", "1", "1", "1"};
    auto device = med::MedicalDeviceFactory::create(deviceConfig);
    if (!device) {
        why = device.message();
        return device.status();
    }
    m.readback = device.take();

    med::Status status = m.actuator->start();
    if (status != med::Status::Ok) {
        why = "the mechanism did not accept the dead-man window: " + m.actuator->lastError();
        return status;
    }
    status = m.readback->start();
    if (status != med::Status::Ok) {
        why = "no readback channel: " + m.readback->lastError();
        return status;
    }
    // Two seconds for a readback that shows the mechanism at rest, heartbeating
    // meanwhile so the window does not run out during the wait.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        m.actuator->heartbeat();
        auto frame = m.readback->read(std::chrono::milliseconds(150));
        if (!frame) {
            continue;
        }
        first = decode(frame.value());
        if (!first.motor && !first.tripped) {
            return med::Status::Ok;
        }
    }
    why = "no readback showed the mechanism at rest";
    return med::Status::Timeout;
}

}  // namespace

int main(int argc, char** argv) {
    std::string configPath = kDefaultConfigPath;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if ((argument == "--config" || argument == "-c") && i + 1 < argc) {
            configPath = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--config %s]\n", argv[0], kDefaultConfigPath);
            return 2;
        }
    }

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = onSignal;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    med::MedicalLogger& logger = med::MedicalLogger::instance();
    med::LoggerConfig loggerConfig;
    loggerConfig.component = kComponent;
    loggerConfig.deviceId = readMachineId();
    loggerConfig.softwareVersion = med::frameworkVersion();
    loggerConfig.chainStatePath = "/var/lib/medplatform/infusion-audit-chain.state";
    logger.configure(loggerConfig);
    logger.audit(med::AuditEvent::SystemStart, "infusion control service starting",
                 {{"config", configPath}, {"framework", med::frameworkVersion()}});

    // 1. The sealed configuration.
    auto loaded = med::MedicalConfiguration::load(configPath, true);
    if (!loaded) {
        logger.audit(med::AuditEvent::SelfTestFailed, "configuration could not be loaded or verified",
                     {{"config", configPath}, {"detail", loaded.message()}});
        std::fprintf(stderr, "infusion-control-service: %s\n", loaded.message().c_str());
        return 1;
    }
    std::vector<std::string> problems;
    auto parsed = infusion::loadServiceConfig(*loaded.value(), &problems);
    if (!parsed) {
        for (const std::string& p : problems) {
            logger.audit(med::AuditEvent::SafetyLimitViolation, "configuration refused",
                         {{"config", configPath}, {"detail", p}});
            std::fprintf(stderr, "infusion-control-service: %s\n", p.c_str());
        }
        return 1;
    }
    const infusion::ServiceConfig cfg = parsed.take();

    // 2. The mechanism, and its self test.
    Mechanism mechanism;
    infusion::Readback first;
    std::string why;
    if (connectMechanism(cfg, mechanism, first, why) != med::Status::Ok) {
        logger.audit(med::AuditEvent::SelfTestFailed, "mechanism self test failed", {{"detail", why}});
        std::fprintf(stderr, "infusion-control-service: %s\n", why.c_str());
        return 1;
    }
    logger.audit(med::AuditEvent::SelfTestPassed, "mechanism self test passed",
                 {{"deadman_ms", std::to_string(mechanism.actuator->info().deadmanWindow.count())}});

    ActuatorAdapter actuatorPort(mechanism.actuator);
    AuditAdapter auditPort;
    infusion::Controller controller(cfg.library, cfg.parameters, actuatorPort, auditPort);
    controller.onReadback(first, infusion::Clock::now());

    // 3. No automatic resumption.
    if (::access(cfg.therapyMarker.c_str(), F_OK) == 0) {
        controller.therapyWasInterrupted(infusion::Clock::now());
        ::unlink(cfg.therapyMarker.c_str());
    }

    // The operator's channel.
    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = cfg.controlSocket;
    auto server = med::MedicalIpcServer::listen(endpoint);
    if (!server) {
        logger.audit(med::AuditEvent::DeviceFault, "control socket could not be created",
                     {{"socket", cfg.controlSocket}, {"detail", server.message()}});
        return 1;
    }
    std::vector<std::unique_ptr<med::MedicalIpcChannel>> clients;

    // 4. Ready.
    med::MedicalUpdate::reportReady();

    bool markerPresent = false;
    std::string lastStatus;
    auto nextStatus = infusion::Clock::now();

    auto handle = [&](const std::map<std::string, std::string>& request) -> infusion::Reply {
        const auto now = infusion::Clock::now();
        auto get = [&](const char* key) {
            const auto it = request.find(key);
            return it == request.end() ? std::string() : it->second;
        };
        const std::string verb = get("verb");
        if (verb == "program") {
            double rate = 0.0, vtbi = 0.0;
            if (!parseNumber(get("rate_ml_h"), rate) || !parseNumber(get("vtbi_ml"), vtbi)) {
                return {false, "rate_ml_h and vtbi_ml must be numbers"};
            }
            return controller.program(get("drug"), get("syringe"), rate, vtbi, get("confirm") == "1",
                                      get("operator"));
        }
        if (verb == "start") return controller.start(now);
        if (verb == "pause") return controller.pause(now);
        if (verb == "resume") return controller.resume(now);
        if (verb == "stop") return controller.stop(now);
        if (verb == "silence") return controller.silence(get("operator"), now);
        if (verb == "acknowledge") {
            if (controller.needsReconnect()) {
                // A lost or tripped mechanism comes back only on an operator's
                // action, never by itself: reconnecting re-arms the dead-man
                // window and finds the mechanism at rest.
                infusion::Readback again;
                std::string reason;
                if (connectMechanism(cfg, mechanism, again, reason) == med::Status::Ok) {
                    controller.linkRestored(infusion::Clock::now());
                    controller.onReadback(again, infusion::Clock::now());
                } else {
                    return {false, "the mechanism is still not answering: " + reason};
                }
            }
            return controller.acknowledge(get("operator"), infusion::Clock::now());
        }
        if (verb == "status") {
            std::string alarms;
            for (const auto& a : controller.alarms()) {
                alarms += (alarms.empty() ? "" : ",") + std::string(infusion::toString(a.kind));
            }
            return {true, std::string(infusion::toString(controller.state())) + " " +
                              (alarms.empty() ? "none" : alarms)};
        }
        return {false, "unknown verb: " + verb};
    };

    while (!gStop.load()) {
        const auto now = infusion::Clock::now();
        auto frame = mechanism.readback->read(std::chrono::milliseconds(10));
        if (frame) {
            controller.onReadback(decode(frame.value()), infusion::Clock::now());
        } else if (frame.status() != med::Status::Timeout) {
            // The readback channel is gone: the controller's supervision raises
            // the alarm; this only keeps the loop from spinning meanwhile.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        controller.tick(infusion::Clock::now());

        auto accepted = server.value()->accept(std::chrono::milliseconds(0));
        if (accepted) {
            clients.push_back(accepted.take());
        }
        for (auto it = clients.begin(); it != clients.end();) {
            unsigned char buffer[1024];
            auto received = (*it)->receive(buffer, sizeof(buffer), std::chrono::milliseconds(0));
            if (!received) {
                if (received.status() == med::Status::Timeout) {
                    ++it;
                } else {
                    it = clients.erase(it);
                }
                continue;
            }
            auto request = med::amp::decodeControl(buffer, received.value());
            const infusion::Reply reply =
                request ? handle(request.value()) : infusion::Reply{false, request.message()};
            const auto ack = med::amp::encodeControlAck(reply.ok ? 0 : 1, reply.detail);
            (*it)->send(ack.data(), ack.size());
            ++it;
        }

        // The marker follows the therapy, so a restart finds what the last run
        // left unfinished.
        if (controller.therapyActive() != markerPresent) {
            if (controller.therapyActive()) {
                markerPresent = writeAtomically(cfg.therapyMarker, controller.currentProgram().drug + "\n");
            } else {
                ::unlink(cfg.therapyMarker.c_str());
                markerPresent = false;
            }
        }

        const std::string status = controller.statusText();
        if (status != lastStatus || now >= nextStatus) {
            writeAtomically(cfg.statusPath, status);
            lastStatus = status;
            nextStatus = now + std::chrono::milliseconds(500);
        }
    }

    // Stopped with a therapy running: the mechanism rests, and the marker stays
    // - the next run must find the therapy interrupted, not finished.
    mechanism.actuator->safeState();
    mechanism.actuator->stop();
    logger.audit(med::AuditEvent::SystemStop, "infusion control service stopping",
                 {{"therapy_active", controller.therapyActive() ? "1" : "0"}});
    return 0;
}
