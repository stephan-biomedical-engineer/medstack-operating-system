// SPDX-License-Identifier: MIT

#include "simulator.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>

#include "mechanism.h"
#include "syringe_model.h"

namespace infusion {

const char* const kReadbackUnits[kReadbackChannels] = {"mm", "mmHg", "1", "1", "1"};

namespace {

using Clock = std::chrono::steady_clock;
using Channel = std::unique_ptr<med::MedicalIpcChannel>;

/// How often the loop wakes: the model's step and the dead-man check are
/// this fine. Ten times finer than the shortest window the mechanism accepts.
constexpr std::chrono::milliseconds kTick{5};

med::Result<std::unique_ptr<med::MedicalIpcServer>> listenOn(const std::string& path,
                                                             unsigned int mode) {
    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = path;
    endpoint.socketMode = mode;
    return med::MedicalIpcServer::listen(endpoint);
}

/// Take a client waiting on `server`, if any. A newcomer replaces the
/// current client when `replace` is set, and is turned away otherwise.
void acceptOne(med::MedicalIpcServer& server, Channel& current, bool replace, bool& arrived) {
    arrived = false;
    med::Result<Channel> accepted = server.accept(std::chrono::milliseconds(0));
    if (!accepted) {
        return;
    }
    if (current && !replace) {
        return;  // closed as it goes out of scope: one controller at a time
    }
    current = accepted.take();
    arrived = true;
}

void answer(med::MedicalIpcChannel& channel, const Mechanism::Answer& a) {
    const std::vector<unsigned char> ack = med::amp::encodeControlAck(a.rejected, a.detail);
    channel.send(ack.data(), ack.size());
}

struct Faults {
    bool silent = false;
};

/// One fault request. Returns the answer to send back.
Mechanism::Answer applyFault(const std::map<std::string, std::string>& request,
                             SyringeModel& model, Faults& faults) {
    std::uint16_t index = 0;
    for (const auto& entry : request) {
        ++index;
        const std::string& key = entry.first;
        const std::string& value = entry.second;
        const bool on = value == "1";
        if (key != "drift" && value != "0" && value != "1") {
            return {index, "expected 0 or 1"};
        }
        if (key == "occlusion") {
            model.setOcclusion(on);
        } else if (key == "air") {
            model.setAir(on);
        } else if (key == "stall") {
            model.setStall(on);
        } else if (key == "silent") {
            faults.silent = on;
        } else if (key == "drift") {
            char* end = nullptr;
            const double fraction = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || !std::isfinite(fraction)) {
                return {index, "expected a fraction"};
            }
            model.setEncoderDrift(fraction);
        } else {
            return {index, "unknown fault"};
        }
    }
    return {};
}

}  // namespace

int runSimulator(const SimulatorConfig& config, const std::atomic<bool>& stop,
                 std::string* error) {
    const Syringe* syringe = findSyringe(config.syringe);
    if (syringe == nullptr) {
        if (error) *error = "unknown syringe '" + config.syringe + "'";
        return 1;
    }
    if (!(config.frameRateHz > 0.0)) {
        if (error) *error = "the frame rate must be positive";
        return 1;
    }

    auto command = listenOn(config.commandPath, config.socketMode);
    auto data = listenOn(config.dataPath, config.socketMode);
    auto fault = listenOn(config.faultPath, 0600);
    for (auto* server : {&command, &data, &fault}) {
        if (!*server) {
            if (error) *error = server->message();
            return 1;
        }
    }

    SyringeModel model(*syringe);
    Mechanism mechanism(model);
    Faults faults;

    Channel controller;
    Channel reader;
    Channel injector;

    const auto framePeriod = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / config.frameRateHz));
    auto last = Clock::now();
    auto nextFrame = last;
    std::uint64_t sequence = 0;

    while (!stop.load()) {
        const auto now = Clock::now();
        model.step(std::chrono::duration<double>(now - last).count());
        last = now;

        // The controller. A hung producer reads nothing, so its socket fills
        // and its timer does not run: that is what "silent" simulates.
        while (controller && !faults.silent) {
            unsigned char buffer[1024];
            med::Result<std::size_t> received =
                controller->receive(buffer, sizeof(buffer), std::chrono::milliseconds(0));
            if (!received) {
                if (received.status() != med::Status::Timeout) {
                    controller.reset();
                    mechanism.disconnected();
                }
                break;
            }
            med::Result<std::map<std::string, std::string>> request =
                med::amp::decodeControl(buffer, received.value());
            if (!request) {
                // Not the protocol: the session ends, and the mechanism rests.
                controller.reset();
                mechanism.disconnected();
                break;
            }
            answer(*controller, mechanism.handle(request.value(), Clock::now()));
        }
        if (!faults.silent) {
            mechanism.tick(Clock::now());
        }

        // Accepted after the controller's messages were read, so a controller
        // that closed and reconnected in the same tick is seen leaving before
        // its successor arrives - otherwise the successor would be turned away
        // as a second controller.
        bool arrived = false;
        acceptOne(*command.value(), controller, false, arrived);
        if (arrived) {
            mechanism.connected(Clock::now());
        }
        acceptOne(*data.value(), reader, true, arrived);
        acceptOne(*fault.value(), injector, true, arrived);

        while (injector) {
            unsigned char buffer[1024];
            med::Result<std::size_t> received =
                injector->receive(buffer, sizeof(buffer), std::chrono::milliseconds(0));
            if (!received) {
                if (received.status() != med::Status::Timeout) {
                    injector.reset();
                }
                break;
            }
            med::Result<std::map<std::string, std::string>> request =
                med::amp::decodeControl(buffer, received.value());
            if (!request) {
                injector.reset();
                break;
            }
            answer(*injector, applyFault(request.value(), model, faults));
        }

        if (now >= nextFrame) {
            nextFrame += framePeriod;
            if (nextFrame < now) {
                nextFrame = now + framePeriod;  // never a burst to catch up
            }
            if (reader && !faults.silent) {
                med::SampleFrame frame;
                frame.sequence = ++sequence;
                frame.timestamp = med::Clock::now();
                frame.channelCount = kReadbackChannels;
                frame.samplesPerChannel = 1;
                // Micro-units, the frame's convention.
                frame.samples = {model.encoderMm() * 1e6, model.pressureMmHg() * 1e6,
                                 model.air() ? 1e6 : 0.0, model.motorRunning() ? 1e6 : 0.0,
                                 mechanism.tripped() ? 1e6 : 0.0};
                std::vector<unsigned char> wire;
                med::amp::encodeFrame(frame, config.frameRateHz, 1000, wire);
                const med::Status sent = reader->send(wire.data(), wire.size());
                if (sent != med::Status::Ok && sent != med::Status::WouldBlock) {
                    reader.reset();
                }
            }
        }

        std::this_thread::sleep_for(kTick);
    }

    mechanism.disconnected();
    return 0;
}

}  // namespace infusion
