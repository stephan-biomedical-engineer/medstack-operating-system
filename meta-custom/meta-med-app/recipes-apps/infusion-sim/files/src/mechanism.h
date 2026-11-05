// SPDX-License-Identifier: MIT
//
// infusion-sim - the producer's side of the actuator protocol.
//
// The framework's MedicalActuator (controller side) states a dead-man window
// and requires it to be accepted; this is the side that accepts it and keeps
// the timer. It is the half that the framework cannot implement across a link,
// and the half the hazard analysis leans on hardest (INF-09,
// docs/REQUISITOS_INFUSAO.md): a controller that has hung cannot stop the
// motor, so the mechanism stops itself.
//
// The rules, in order of the session:
//
//   1. The first message must state the window (deadman_ms). Until one is
//      accepted, nothing else is: a mechanism with no timer must not move.
//   2. After it, the mechanism is armed and AT REST. Arming never starts it.
//   3. Every message is a proof of life. Silence for longer than the window
//      brings the mechanism to rest and TRIPS the session: from then on every
//      request is refused until the controller reconnects and arms again. A
//      trip is never undone by the next heartbeat, because a controller that
//      comes back from a hang must not find the motor willing to resume what
//      it was doing (INF-11, no automatic resumption).
//   4. A controller that disconnects brings it to rest at once.
//
// Time is passed in, never read: a test drives a session of minutes in
// microseconds and puts the deadline exactly where it wants it.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "syringe_model.h"

namespace infusion {

class Mechanism {
public:
    using Clock = std::chrono::steady_clock;

    /// The setpoint key this mechanism takes, mL/h.
    static constexpr const char* kRateKey = "rate_ml_h";

    struct Answer {
        /// 0 accepted; otherwise 1 + the index of the refused key, in the
        /// order the message's map iterates (the control protocol's rule).
        std::uint16_t rejected = 0;
        std::string detail;
    };

    struct Limits {
        double maxRateMlH = 1200.0;
        std::chrono::milliseconds minWindow{50};
        std::chrono::milliseconds maxWindow{5000};
    };

    explicit Mechanism(SyringeModel& model) : Mechanism(model, Limits{}) {}
    Mechanism(SyringeModel& model, Limits limits) : model_(model), limits_(limits) {}

    /// A controller connected. The previous session, if any, is over.
    void connected(Clock::time_point now);
    /// The controller went away: at rest, now.
    void disconnected();

    Answer handle(const std::map<std::string, std::string>& message, Clock::time_point now);

    /// Check the dead-man window. Called by the owner of the clock, often.
    void tick(Clock::time_point now);

    bool armed() const { return armed_; }
    bool tripped() const { return tripped_; }
    std::chrono::milliseconds window() const { return window_; }

private:
    void rest() { model_.setRate(0.0); }
    static Answer refuse(std::uint16_t index, std::string detail) {
        return Answer{index, std::move(detail)};
    }

    SyringeModel& model_;
    Limits limits_;
    bool connected_ = false;
    bool armed_ = false;
    bool tripped_ = false;
    std::chrono::milliseconds window_{0};
    Clock::time_point lastHeard_{};
};

}  // namespace infusion
