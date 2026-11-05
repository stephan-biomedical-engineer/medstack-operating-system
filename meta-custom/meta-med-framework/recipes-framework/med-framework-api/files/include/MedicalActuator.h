// SPDX-License-Identifier: MIT
//
// MedFramework - actuator abstraction.
//
// A sensor and an actuator fail in opposite directions. A sensor that stops
// stops informing; the hazard is a record that is wrong or missing. An actuator
// that stops has to stop IN A DEFINED STATE, and one that keeps going after its
// controller died keeps acting on the patient. That is why this is an interface
// of its own and not three more methods on MedicalDevice
// (decision D3 of the second device profile's plan): the contract that matters here -
// who brings the mechanism to rest, and when - has no counterpart in a sensor.
//
// What an actuator is, to this layer:
//
//   * a producer on the far side of a message channel, like a MedicalDevice
//     front-end - the firmware of a real-time core, or a process standing in
//     for a mechanism - which takes SETPOINTS and answers each one;
//   * which keeps a DEAD-MAN TIMER of its own. The controller proves it is alive
//     with heartbeat() at least once per heartbeatPeriod; a producer that hears
//     nothing for heartbeatPeriod x missedHeartbeats goes to its safe state by
//     itself. The safe state belongs on the producer's side because a
//     controller that has hung cannot stop anything - and the framework cannot
//     implement a timer on the far side of a link, so it does the next best
//     thing: start() states the window to the producer and REQUIRES it to be
//     accepted. A producer that does not know what a dead-man window is
//     refuses the key, as the control protocol obliges it to, and the actuator
//     is never started.
//
// What it is not: a readback. What the mechanism actually does - a position, a
// speed, a pressure - is a measurement, and measurements come in through a
// MedicalDevice, usually on the same producer. An actuator that reported its
// own result would be grading itself; the readback is only evidence when it
// comes from the measurement path.
//
// Setpoints, like MedicalDevice options, are opaque key/value strings. Which
// quantities a mechanism takes is the application's knowledge and how each is
// spelled is the producer's; a `rate` field here would be the framework
// acquiring the vocabulary of one class of device.
//
// Drivers shipped with the framework, named after the transport:
//
//   "seqpacket" - a producer process listening on an AF_UNIX SOCK_SEQPACKET
//                 socket, speaking med_amp_abi.h's control message and
//                 acknowledgement.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "MedicalTypes.h"

namespace med {

/// Keys the actuator protocol reserves inside a control message. A setpoint
/// or a driver option may not use them: they are how the framework speaks to
/// the producer's dead-man timer, and an application that could send one
/// could also disarm it.
namespace actuation {
/// The producer's dead-man window, in milliseconds, sent by start().
constexpr const char* kDeadmanKey = "deadman_ms";
/// A proof of life, with a counter that only increases.
constexpr const char* kHeartbeatKey = "heartbeat";
/// Bring the mechanism to its safe state now.
constexpr const char* kSafeStateKey = "safe_state";

bool isReservedKey(const std::string& key) noexcept;
}  // namespace actuation

struct ActuatorConfig {
    std::string driver = "seqpacket";
    std::string id = "actuator0";
    /// The producer's channel: for seqpacket, the path of its socket.
    /// Required; there is no default, as for every driver with an address.
    std::string address;
    /// The controller promises a heartbeat at least this often...
    std::chrono::milliseconds heartbeatPeriod{200};
    /// ...and the producer gives up after this many missed. The product is the
    /// dead-man window start() states, and the longest time the mechanism can
    /// go on acting after its controller died.
    std::uint32_t missedHeartbeats = 3;
    /// Sent with the dead-man window, in the same message, and judged by the
    /// producer. At most kControlMaxOptions - 1, because the window takes one.
    std::map<std::string, std::string> driverOptions;
};

struct ActuatorInfo {
    std::string id;
    std::string model;
    std::string driver;
    std::string address;
    /// heartbeatPeriod x missedHeartbeats, as accepted by the producer.
    std::chrono::milliseconds deadmanWindow{0};
};

/// Not thread safe: one controller loop owns an actuator, and calls heartbeat()
/// from the same loop that commands it.
///
/// Every exchange is answered or it is the end of the session. An answer that
/// does not arrive in time may still arrive later, and a late answer read as
/// the answer to the NEXT request is how a refusal turns into an acceptance;
/// so after a Timeout, or anything on the channel that is not an answer, every
/// call returns Unavailable until stop() and start() - and start() finds a
/// producer that has, by then, gone to its safe state on its own.
class MedicalActuator {
public:
    virtual ~MedicalActuator() = default;

    virtual ActuatorInfo info() const = 0;

    /// Connect, state the dead-man window and the driver options, and require
    /// the producer to accept all of them. InvalidArgument when it refuses one
    /// (lastError() says which and why), Timeout when it does not answer. The
    /// producer is in its safe state after an accepted start(): starting an
    /// actuator never sets it acting.
    virtual Status start() = 0;

    /// Bring the mechanism to its safe state, then close the channel. Closing
    /// is never the only safeguard - a producer that missed the request still
    /// stops when its dead-man window runs out.
    virtual Status stop() = 0;

    virtual bool isRunning() const = 0;

    /// Send one setpoint and wait for the producer's answer. InvalidArgument
    /// for a setpoint that is empty, uses a reserved key or does not fit a
    /// control message (refused here, before anything is sent), or that the
    /// producer refused.
    virtual Status command(const std::map<std::string, std::string>& setpoint) = 0;

    /// Prove the controller is alive. Must be called at least once per
    /// heartbeatPeriod for as long as the mechanism is allowed to act.
    virtual Status heartbeat() = 0;

    /// Bring the mechanism to its safe state now, and wait for the answer.
    virtual Status safeState() = 0;

    /// What the last failed call refused, in words, for the audit record.
    virtual std::string lastError() const { return {}; }
};

class MedicalActuatorFactory {
public:
    using Constructor =
        std::function<Result<std::unique_ptr<MedicalActuator>>(const ActuatorConfig&)>;

    /// Returns true if the name was free; an existing name is never replaced.
    static bool registerDriver(const std::string& name, Constructor constructor);

    /// Refuses, before any driver is consulted: an empty address, a heartbeat
    /// period that is not positive, zero missed heartbeats, and options that
    /// use a reserved key or do not fit beside the dead-man window.
    static Result<std::unique_ptr<MedicalActuator>> create(const ActuatorConfig& config);

    static std::vector<std::string> drivers();
};

}  // namespace med
