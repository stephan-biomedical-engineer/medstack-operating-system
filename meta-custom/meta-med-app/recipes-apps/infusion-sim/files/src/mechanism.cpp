// SPDX-License-Identifier: MIT

#include "mechanism.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

#include <medplatform/MedicalActuator.h>

namespace infusion {

namespace {

/// A whole, positive decimal and nothing else. "600", never "600ms" or " 600".
bool parseWhole(const std::string& text, long long& out) {
    if (text.empty() || text.size() > 9) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    out = std::atoll(text.c_str());
    return true;
}

/// A finite decimal and nothing else.
bool parseDecimal(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

void Mechanism::connected(Clock::time_point now) {
    rest();
    connected_ = true;
    armed_ = false;
    tripped_ = false;
    window_ = std::chrono::milliseconds(0);
    lastHeard_ = now;
}

void Mechanism::disconnected() {
    rest();
    connected_ = false;
    armed_ = false;
}

Mechanism::Answer Mechanism::handle(const std::map<std::string, std::string>& message,
                                    Clock::time_point now) {
    using med::actuation::kDeadmanKey;
    using med::actuation::kHeartbeatKey;
    using med::actuation::kSafeStateKey;

    if (!connected_) {
        return refuse(1, "no controller session");
    }

    if (!armed_) {
        // Rule 1: the window first, and the only thing the first message may
        // carry besides it is nothing - this mechanism takes no options.
        std::uint16_t index = 0;
        long long ms = -1;
        bool sawWindow = false;
        for (const auto& entry : message) {
            ++index;
            if (entry.first != kDeadmanKey) {
                return refuse(index, sawWindow ? "unknown option" : "deadman_ms required first");
            }
            sawWindow = true;
            if (!parseWhole(entry.second, ms) ||
                std::chrono::milliseconds(ms) < limits_.minWindow ||
                std::chrono::milliseconds(ms) > limits_.maxWindow) {
                return refuse(index, "deadman_ms out of range");
            }
        }
        if (!sawWindow) {
            return refuse(1, "deadman_ms required first");
        }
        window_ = std::chrono::milliseconds(ms);
        armed_ = true;
        lastHeard_ = now;
        rest();  // rule 2
        return {};
    }

    if (tripped_) {
        // Rule 3: nothing is accepted after a trip, not even a heartbeat.
        return refuse(1, "dead-man tripped; reconnect");
    }

    // Every well-formed message is a proof of life, a refused one included:
    // the controller that sent it is running.
    lastHeard_ = now;

    // Validate the whole message before applying any of it: a refused message
    // has no effect at all, or "refused" would mean "half done".
    bool toRest = false;
    bool setRate = false;
    double rate = 0.0;
    std::uint16_t index = 0;
    for (const auto& entry : message) {
        ++index;
        const std::string& key = entry.first;
        if (key == kHeartbeatKey) {
            continue;
        }
        if (key == kSafeStateKey) {
            toRest = true;
            continue;
        }
        if (key == kRateKey) {
            if (!parseDecimal(entry.second, rate) || rate < 0.0 || rate > limits_.maxRateMlH) {
                return refuse(index, "rate out of the mechanism's range");
            }
            setRate = true;
            continue;
        }
        if (key == kDeadmanKey) {
            return refuse(index, "already armed");
        }
        return refuse(index, "unknown setpoint");
    }
    // Rest wins over a rate in the same message.
    if (toRest) {
        rest();
    } else if (setRate) {
        model_.setRate(rate);
    }
    return {};
}

void Mechanism::tick(Clock::time_point now) {
    if (!armed_ || tripped_) {
        return;
    }
    if (now - lastHeard_ > window_) {
        rest();
        tripped_ = true;
    }
}

}  // namespace infusion
