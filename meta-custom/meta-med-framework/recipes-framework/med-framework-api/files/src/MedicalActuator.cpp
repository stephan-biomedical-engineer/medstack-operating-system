// SPDX-License-Identifier: MIT
//
// MedFramework - actuator drivers. See MedicalActuator.h for the contract.

#include "MedicalActuator.h"

#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "MedicalDevice.h"
#include "MedicalIPC.h"

namespace med {

namespace actuation {

bool isReservedKey(const std::string& key) noexcept {
    return key == kDeadmanKey || key == kHeartbeatKey || key == kSafeStateKey;
}

}  // namespace actuation

namespace {

/// The dead-man window, as the producer is told it.
std::chrono::milliseconds deadmanWindow(const ActuatorConfig& config) {
    return config.heartbeatPeriod * static_cast<std::int64_t>(config.missedHeartbeats);
}

/// An actuator behind a producer on a message channel. The transport is a
/// parameter for the same reason as LinkDevice's in MedicalDevice.cpp: a
/// mechanism on a co-processor and one simulated by a process must be held to
/// one protocol, by one implementation. Only "seqpacket" is registered today;
/// the co-processor's link is a later phase of the second device profile's
/// plan, and a driver nothing exercises is not registered ahead of it.
class LinkActuator : public MedicalActuator {
public:
    LinkActuator(const ActuatorConfig& config, IpcTransport transport, const char* name,
                 const char* model)
        : config_(config), transport_(transport), name_(name), model_(model) {}

    ~LinkActuator() override { stop(); }

    ActuatorInfo info() const override {
        ActuatorInfo info;
        info.id = config_.id;
        info.model = model_;
        info.driver = name_;
        info.address = config_.address;
        info.deadmanWindow = deadmanWindow(config_);
        return info;
    }

    Status start() override {
        if (channel_) {
            return desynchronised_ ? unavailable() : Status::Ok;
        }

        IpcEndpoint endpoint;
        endpoint.transport = transport_;
        endpoint.address = config_.address;

        // The same bounded window as a sensor's producer: one that comes up
        // after the controller is normal, one that never comes is a failure
        // reported while someone is still looking.
        const auto deadline = std::chrono::steady_clock::now() + kConnectWindow;
        for (;;) {
            Result<std::unique_ptr<MedicalIpcChannel>> channel =
                MedicalIpcChannel::connect(endpoint);
            if (channel) {
                channel_ = channel.take();
                break;
            }
            lastError_ = channel.message();
            if (std::chrono::steady_clock::now() >= deadline) {
                return channel.status();
            }
            std::this_thread::sleep_for(kConnectRetryInterval);
        }
        desynchronised_ = false;

        // The window and the options go in ONE message, so the producer cannot
        // accept the options and miss the window: it accepts both or neither.
        std::map<std::string, std::string> first = config_.driverOptions;
        first[actuation::kDeadmanKey] = std::to_string(deadmanWindow(config_).count());
        const Status accepted = exchange(first, kStartTimeout);
        if (accepted != Status::Ok) {
            channel_.reset();
            desynchronised_ = false;
        }
        return accepted;
    }

    Status stop() override {
        if (!channel_) {
            return Status::Ok;
        }
        const Status rested = desynchronised_ ? Status::Unavailable : safeState();
        channel_.reset();
        desynchronised_ = false;
        return rested;
    }

    bool isRunning() const override { return static_cast<bool>(channel_) && !desynchronised_; }

    Status command(const std::map<std::string, std::string>& setpoint) override {
        if (setpoint.empty()) {
            lastError_ = "an empty setpoint asks the mechanism for nothing";
            return Status::InvalidArgument;
        }
        for (const auto& entry : setpoint) {
            if (actuation::isReservedKey(entry.first)) {
                lastError_ = "setpoint key '" + entry.first + "' is reserved by the actuator protocol";
                return Status::InvalidArgument;
            }
        }
        return exchange(setpoint, config_.heartbeatPeriod);
    }

    Status heartbeat() override {
        return exchange({{actuation::kHeartbeatKey, std::to_string(++heartbeats_)}},
                        config_.heartbeatPeriod);
    }

    Status safeState() override {
        return exchange({{actuation::kSafeStateKey, "1"}}, config_.heartbeatPeriod);
    }

    std::string lastError() const override { return lastError_; }

private:
    Status unavailable() {
        lastError_ = "an earlier exchange went unanswered or was answered with something "
                     "else; the session is over until stop() and start()";
        return Status::Unavailable;
    }

    /// One request, one answer, within `timeout`. Anything but an answer in
    /// time ends the session (MedicalActuator.h says why); a refusal is an
    /// answer and does not.
    Status exchange(const std::map<std::string, std::string>& message,
                    std::chrono::milliseconds timeout) {
        if (!channel_) {
            lastError_ = "the actuator is not started";
            return Status::Unavailable;
        }
        if (desynchronised_) {
            return unavailable();
        }

        Result<std::vector<unsigned char>> encoded = amp::encodeControl(message);
        if (!encoded) {
            lastError_ = encoded.message();
            return encoded.status();
        }

        const Status sent = channel_->send(encoded.value().data(), encoded.value().size());
        if (sent != Status::Ok) {
            desynchronised_ = true;
            lastError_ = "the request could not be sent to the producer";
            return sent;
        }

        unsigned char buffer[512];
        Result<std::size_t> received = channel_->receive(buffer, sizeof(buffer), timeout);
        if (!received) {
            desynchronised_ = true;
            lastError_ = "the producer did not answer within " +
                         std::to_string(timeout.count()) + " ms: " + received.message();
            return received.status() == Status::Timeout ? Status::Timeout : received.status();
        }

        Result<amp::ControlAck> ack = amp::decodeControlAck(buffer, received.value());
        if (!ack) {
            desynchronised_ = true;
            lastError_ = "the producer answered with something that is not an answer: " +
                         ack.message();
            return Status::IntegrityError;
        }
        if (ack.value().rejectedIndex != 0) {
            // In the order the map iterates, which is the order encodeControl
            // wrote the options in.
            std::size_t index = ack.value().rejectedIndex - 1u;
            std::string key = "?";
            for (const auto& entry : message) {
                if (index-- == 0) {
                    key = entry.first;
                    break;
                }
            }
            lastError_ = "the producer refused '" + key + "': " + std::string(ack.value().detail);
            return Status::InvalidArgument;
        }
        return Status::Ok;
    }

    static constexpr std::chrono::milliseconds kConnectWindow{5000};
    static constexpr std::chrono::milliseconds kConnectRetryInterval{250};
    /// The first answer may take longer than a heartbeat: the producer is
    /// bringing its mechanism to rest and arming its timer.
    static constexpr std::chrono::milliseconds kStartTimeout{1000};

    ActuatorConfig config_;
    IpcTransport transport_;
    std::string name_;
    std::string model_;
    std::unique_ptr<MedicalIpcChannel> channel_;
    bool desynchronised_ = false;
    std::uint64_t heartbeats_ = 0;
    std::string lastError_;
};

struct Registry {
    std::mutex mutex;
    std::map<std::string, MedicalActuatorFactory::Constructor> constructors;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

const bool kBuiltinActuatorsRegistered = [] {
    MedicalActuatorFactory::registerDriver("seqpacket", [](const ActuatorConfig& config) {
        return Result<std::unique_ptr<MedicalActuator>>::ok(std::unique_ptr<MedicalActuator>(
            new LinkActuator(config, IpcTransport::UnixSeqpacket, "seqpacket",
                             "MedPlatform actuator link (seqpacket)")));
    });
    return true;
}();

}  // namespace

bool MedicalActuatorFactory::registerDriver(const std::string& name, Constructor constructor) {
    if (name.empty() || !constructor) {
        return false;
    }
    Registry& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mutex);
    if (reg.constructors.find(name) != reg.constructors.end()) {
        return false;
    }
    reg.constructors[name] = std::move(constructor);
    return true;
}

Result<std::unique_ptr<MedicalActuator>> MedicalActuatorFactory::create(
    const ActuatorConfig& config) {
    using ActuatorResult = Result<std::unique_ptr<MedicalActuator>>;

    (void)kBuiltinActuatorsRegistered;

    if (config.address.empty()) {
        return ActuatorResult::fail(Status::InvalidArgument,
                                    "an actuator needs an address: the producer's channel");
    }
    if (config.heartbeatPeriod.count() <= 0) {
        return ActuatorResult::fail(Status::InvalidArgument,
                                    "heartbeatPeriod must be positive");
    }
    if (config.missedHeartbeats == 0) {
        // A window of zero is a producer that stops before the first heartbeat
        // could reach it - or, read the other way, one with no timer at all.
        return ActuatorResult::fail(Status::InvalidArgument,
                                    "missedHeartbeats must be at least 1");
    }
    for (const auto& entry : config.driverOptions) {
        if (actuation::isReservedKey(entry.first)) {
            return ActuatorResult::fail(
                Status::InvalidArgument,
                "option '" + entry.first + "' is reserved by the actuator protocol");
        }
    }
    // Checked as start() will send it, so a configuration that could never be
    // sent fails here, identically on every target.
    std::map<std::string, std::string> first = config.driverOptions;
    first[actuation::kDeadmanKey] = std::to_string(deadmanWindow(config).count());
    Result<std::vector<unsigned char>> encoded = amp::encodeControl(first);
    if (!encoded) {
        return ActuatorResult::fail(encoded.status(),
                                    "the options do not fit beside the dead-man window: " +
                                        encoded.message());
    }

    Constructor constructor;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> guard(reg.mutex);
        const auto it = reg.constructors.find(config.driver);
        if (it == reg.constructors.end()) {
            return ActuatorResult::fail(Status::NotFound, "no MedicalActuator driver named '" +
                                                              config.driver + "'");
        }
        constructor = it->second;
    }
    return constructor(config);
}

std::vector<std::string> MedicalActuatorFactory::drivers() {
    (void)kBuiltinActuatorsRegistered;
    Registry& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mutex);
    std::vector<std::string> names;
    for (const auto& entry : reg.constructors) {
        names.push_back(entry.first);
    }
    return names;
}

}  // namespace med
