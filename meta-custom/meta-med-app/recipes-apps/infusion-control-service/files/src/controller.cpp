// SPDX-License-Identifier: MIT

#include "controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace infusion {

namespace {

std::string num(double value, int decimals = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

double seconds(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

}  // namespace

const Drug* Library::drug(const std::string& id) const {
    for (const Drug& d : drugs) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

const SyringeSpec* Library::syringe(const std::string& id) const {
    for (const SyringeSpec& s : syringes) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

const char* toString(State state) noexcept {
    switch (state) {
        case State::Idle:       return "idle";
        case State::Programmed: return "programmed";
        case State::Infusing:   return "infusing";
        case State::Paused:     return "paused";
        case State::Completed:  return "completed";
        case State::Alarm:      return "alarm";
    }
    return "unknown";
}

const char* toString(Priority priority) noexcept {
    switch (priority) {
        case Priority::Low:    return "low";
        case Priority::Medium: return "medium";
        case Priority::High:   return "high";
    }
    return "unknown";
}

const char* toString(AlarmKind kind) noexcept {
    switch (kind) {
        case AlarmKind::Occlusion:          return "occlusion";
        case AlarmKind::Air:                return "air";
        case AlarmKind::DeliveryFault:      return "delivery_fault";
        case AlarmKind::VtbiComplete:       return "vtbi_complete";
        case AlarmKind::ProducerLost:       return "mechanism_lost";
        case AlarmKind::ActuatorFault:      return "actuator_fault";
        case AlarmKind::TherapyInterrupted: return "therapy_interrupted";
    }
    return "unknown";
}

Priority priorityOf(AlarmKind kind) noexcept {
    return kind == AlarmKind::VtbiComplete ? Priority::Medium : Priority::High;
}

Controller::Controller(Library library, Parameters parameters, ActuatorPort& actuator,
                       AuditPort& audit)
    : library_(std::move(library)),
      parameters_(parameters),
      actuator_(actuator),
      audit_(audit) {}

double Controller::mlFromMm(double mm) const {
    return syringe_ == nullptr ? 0.0 : mm * syringe_->areaMm2 / 1000.0;
}

double Controller::deliveredMl() const {
    double ml = deliveredBaseMl_;
    if (stretchRunning_) {
        ml += mlFromMm(last_.encoderMm - stretchStartMm_);
    }
    return ml;
}

bool Controller::active(AlarmKind kind) const {
    return std::any_of(alarms_.begin(), alarms_.end(),
                       [kind](const Alarm& a) { return a.kind == kind; });
}

bool Controller::needsReconnect() const {
    return !linkUp_ || (active(AlarmKind::ActuatorFault) && last_.tripped) ||
           active(AlarmKind::ProducerLost);
}

Reply Controller::refuseIn(const char* verb) const {
    return refuse(std::string(verb) + " is not allowed while " + toString(state_));
}

void Controller::transition(State next) {
    state_ = next;
}

void Controller::restartWindow(Clock::time_point now) {
    windowStart_ = now;
    windowStartMm_ = last_.encoderMm;
}

void Controller::commandRest(Clock::time_point now) {
    // A failure here is not handled here: a mechanism that cannot be told to
    // rest is a lost mechanism, and its own dead-man timer is what stops it.
    (void)actuator_.safeState();
    restSince_ = now;
}

void Controller::toRest(const std::string& why, Clock::time_point now) {
    commandRest(now);
    if (stretchRunning_) {
        deliveredBaseMl_ += mlFromMm(last_.encoderMm - stretchStartMm_);
        stretchRunning_ = false;
    }
    if (state_ == State::Infusing) {
        audit_.audit(med::AuditEvent::TherapyStopped, "infusion interrupted",
                     {{"reason", why}, {"delivered_ml", num(deliveredMl())}});
    }
}

void Controller::endTherapy(const std::string& reason) {
    therapyActive_ = false;
    audit_.audit(med::AuditEvent::TherapyStopped, "therapy ended",
                 {{"reason", reason},
                  {"drug", program_.drug},
                  {"delivered_ml", num(deliveredMl())},
                  {"vtbi_ml", num(program_.vtbiMl)}});
}

void Controller::raise(AlarmKind kind, const std::string& detail, Clock::time_point now) {
    if (active(kind)) {
        return;
    }
    alarms_.push_back(Alarm{kind, detail, false});
    audit_.audit(med::AuditEvent::AlarmRaised, detail,
                 {{"alarm", toString(kind)},
                  {"priority", toString(priorityOf(kind))},
                  {"state", toString(state_)}});
    if (kind == AlarmKind::ProducerLost || kind == AlarmKind::ActuatorFault) {
        audit_.audit(med::AuditEvent::ActuatorFault, detail, {{"alarm", toString(kind)}});
    }
    if (priorityOf(kind) == Priority::High) {
        if (state_ == State::Infusing || state_ == State::Paused) {
            toRest(std::string("alarm:") + toString(kind), now);
        }
        transition(State::Alarm);
    }
}

bool Controller::causePersists(const Alarm& alarm) const {
    switch (alarm.kind) {
        case AlarmKind::Occlusion:
            return last_.pressureMmHg >= parameters_.occlusionMmHg;
        case AlarmKind::Air:
            return last_.air;
        case AlarmKind::DeliveryFault:
            // One-shot, except for a motor still driven with nothing running.
            return last_.motor && state_ != State::Infusing;
        case AlarmKind::ProducerLost:
            return !linkUp_ || !haveReadback_;
        case AlarmKind::ActuatorFault:
            return !linkUp_ || last_.tripped;
        case AlarmKind::VtbiComplete:
        case AlarmKind::TherapyInterrupted:
            return false;
    }
    return true;
}

Reply Controller::program(const std::string& drug, const std::string& syringe, double rateMlH,
                          double vtbiMl, bool confirmSoft, const std::string& operatorName) {
    if (state_ != State::Idle && state_ != State::Programmed && state_ != State::Completed) {
        return refuseIn("program");
    }
    if (!alarms_.empty()) {
        return refuse("acknowledge the alarm first");
    }
    const Drug* d = library_.drug(drug);
    if (d == nullptr) {
        return refuse("drug '" + drug + "' is not in the library");
    }
    const SyringeSpec* s = library_.syringe(syringe);
    if (s == nullptr) {
        return refuse("syringe '" + syringe + "' is not in the library");
    }
    if (!std::isfinite(rateMlH) || rateMlH <= 0.0) {
        return refuse("the rate must be a positive number of mL/h");
    }
    if (rateMlH > parameters_.maxRateMlH) {
        return refuse("the rate is beyond the mechanism's range");
    }
    if (rateMlH > d->hardMaxMlH) {
        // INF-01: refused, and the refusal is a record.
        audit_.audit(med::AuditEvent::SafetyLimitViolation, "rate above the drug's hard limit refused",
                     {{"drug", d->id},
                      {"rate_ml_h", num(rateMlH)},
                      {"hard_max_ml_h", num(d->hardMaxMlH)}});
        return refuse("above the hard limit of " + num(d->hardMaxMlH, 1) + " mL/h for " + d->id);
    }
    std::string overriddenBy;
    if (rateMlH > d->softMaxMlH) {
        if (!confirmSoft) {
            return refuse("above the soft limit of " + num(d->softMaxMlH, 1) +
                          " mL/h: confirm to proceed");
        }
        if (operatorName.empty()) {
            return refuse("a soft limit can only be confirmed by a named operator");
        }
        overriddenBy = operatorName;
    }
    if (!std::isfinite(vtbiMl) || vtbiMl <= 0.0 || vtbiMl > s->volumeMl) {
        return refuse("the volume to be infused must be positive and fit the syringe");
    }

    if (!overriddenBy.empty()) {
        // INF-02: accepted on someone's word, and the trail says whose.
        audit_.audit(med::AuditEvent::SafetyLimitOverridden, "rate above the soft limit confirmed",
                     {{"drug", d->id},
                      {"rate_ml_h", num(rateMlH)},
                      {"soft_max_ml_h", num(d->softMaxMlH)},
                      {"operator", overriddenBy}});
    }
    program_ = Program{d->id, s->id, rateMlH, vtbiMl, overriddenBy};
    syringe_ = s;
    deliveredBaseMl_ = 0.0;
    stretchRunning_ = false;
    transition(State::Programmed);
    audit_.audit(med::AuditEvent::ConfigurationChanged, "therapy programmed",
                 {{"drug", d->id},
                  {"syringe", s->id},
                  {"rate_ml_h", num(rateMlH)},
                  {"vtbi_ml", num(vtbiMl)},
                  {"overridden_by", overriddenBy}});
    return Reply{true, "programmed " + d->id + " " + num(rateMlH, 1) + " mL/h"};
}

Reply Controller::start(Clock::time_point now) {
    if (state_ != State::Programmed) {
        return refuseIn("start");
    }
    if (!linkUp_ || !haveReadback_) {
        return refuse("the mechanism is not answering");
    }
    const med::Status set = actuator_.setRate(program_.rateMlH);
    if (set != med::Status::Ok) {
        const AlarmKind kind = (set == med::Status::InvalidArgument) ? AlarmKind::ActuatorFault
                                                                     : AlarmKind::ProducerLost;
        if (kind == AlarmKind::ProducerLost) {
            linkUp_ = false;
        }
        raise(kind, "the mechanism did not take the rate: " + actuator_.lastError(), now);
        return refuse("the mechanism did not take the rate");
    }
    therapyActive_ = true;
    stretchStartMm_ = last_.encoderMm;
    stretchRunning_ = true;
    restartWindow(now);
    transition(State::Infusing);
    audit_.audit(med::AuditEvent::TherapyStarted, "infusion started",
                 {{"reason", "start"},
                  {"drug", program_.drug},
                  {"syringe", program_.syringe},
                  {"rate_ml_h", num(program_.rateMlH)},
                  {"vtbi_ml", num(program_.vtbiMl)}});
    return Reply{true, "infusing"};
}

Reply Controller::pause(Clock::time_point now) {
    if (state_ != State::Infusing) {
        return refuseIn("pause");
    }
    (void)actuator_.setRate(0.0);
    restSince_ = now;
    deliveredBaseMl_ += mlFromMm(last_.encoderMm - stretchStartMm_);
    stretchRunning_ = false;
    transition(State::Paused);
    audit_.audit(med::AuditEvent::TherapyStopped, "infusion paused",
                 {{"reason", "paused"}, {"delivered_ml", num(deliveredMl())}});
    return Reply{true, "paused"};
}

Reply Controller::resume(Clock::time_point now) {
    if (state_ != State::Paused) {
        return refuseIn("resume");
    }
    if (!linkUp_ || !haveReadback_) {
        return refuse("the mechanism is not answering");
    }
    const med::Status set = actuator_.setRate(program_.rateMlH);
    if (set != med::Status::Ok) {
        if (set != med::Status::InvalidArgument) {
            linkUp_ = false;
        }
        raise(set == med::Status::InvalidArgument ? AlarmKind::ActuatorFault : AlarmKind::ProducerLost,
              "the mechanism did not take the rate: " + actuator_.lastError(), now);
        return refuse("the mechanism did not take the rate");
    }
    stretchStartMm_ = last_.encoderMm;
    stretchRunning_ = true;
    restartWindow(now);
    transition(State::Infusing);
    audit_.audit(med::AuditEvent::TherapyStarted, "infusion resumed",
                 {{"reason", "resumed"}, {"delivered_ml", num(deliveredMl())}});
    return Reply{true, "infusing"};
}

Reply Controller::stop(Clock::time_point now) {
    switch (state_) {
        case State::Idle:
            return refuseIn("stop");
        case State::Programmed:
        case State::Completed:
            program_ = Program{};
            transition(State::Idle);
            return Reply{true, "idle"};
        case State::Infusing:
        case State::Paused:
            commandRest(now);
            if (stretchRunning_) {
                deliveredBaseMl_ += mlFromMm(last_.encoderMm - stretchStartMm_);
                stretchRunning_ = false;
            }
            endTherapy("stopped");
            program_ = Program{};
            transition(State::Idle);
            return Reply{true, "idle"};
        case State::Alarm:
            // The operator abandons the therapy; the alarms stay until
            // acknowledged, and then the device is idle.
            if (therapyActive_) {
                endTherapy("stopped in alarm");
            }
            program_ = Program{};
            return Reply{true, "stopped; acknowledge the alarm"};
    }
    return refuseIn("stop");
}

Reply Controller::acknowledge(const std::string& operatorName, Clock::time_point now) {
    (void)now;
    if (alarms_.empty()) {
        return refuse("no alarm to acknowledge");
    }
    if (operatorName.empty()) {
        return refuse("an acknowledgement needs a named operator");
    }
    std::string names;
    for (const Alarm& a : alarms_) {
        names += (names.empty() ? "" : ",") + std::string(toString(a.kind));
    }
    audit_.audit(med::AuditEvent::AlarmAcknowledged, "alarm acknowledged",
                 {{"operator", operatorName}, {"alarms", names}, {"action", "acknowledge"}});

    std::vector<Alarm> remaining;
    for (const Alarm& a : alarms_) {
        if (causePersists(a)) {
            remaining.push_back(a);
        }
    }
    alarms_ = remaining;
    if (!alarms_.empty()) {
        std::string persist;
        for (const Alarm& a : alarms_) {
            persist += (persist.empty() ? "" : ",") + std::string(toString(a.kind));
        }
        return Reply{false, "acknowledged; the cause persists: " + persist};
    }
    silencedUntil_ = Clock::time_point{};
    if (state_ == State::Alarm) {
        // Never back to infusing: resuming is the operator's decision (INF-11).
        if (therapyActive_) {
            transition(State::Paused);
        } else if (!program_.drug.empty()) {
            transition(State::Programmed);
        } else {
            transition(State::Idle);
        }
    }
    return Reply{true, std::string("acknowledged; ") + toString(state_)};
}

Reply Controller::silence(const std::string& operatorName, Clock::time_point now) {
    if (alarms_.empty()) {
        return refuse("no alarm to silence");
    }
    if (operatorName.empty()) {
        return refuse("silencing needs a named operator");
    }
    silencedUntil_ = now + parameters_.silenceMax;
    for (Alarm& a : alarms_) {
        a.silenced = true;
    }
    audit_.audit(med::AuditEvent::AlarmAcknowledged, "alarm silenced",
                 {{"operator", operatorName},
                  {"action", "silence"},
                  {"seconds", std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                  parameters_.silenceMax).count())}});
    return Reply{true, "silenced"};
}

void Controller::onReadback(const Readback& readback, Clock::time_point now) {
    if (!haveReadback_) {
        // Supervision starts with the first readback: before it, the service
        // is still in its self test.
        haveReadback_ = true;
    }
    last_ = readback;
    lastReadbackAt_ = now;
    supervising_ = true;

    if (readback.tripped) {
        raise(AlarmKind::ActuatorFault, "the mechanism tripped its dead-man timer", now);
    }
    if (readback.motor && state_ != State::Infusing && now - restSince_ > parameters_.restSettle) {
        // INF-05 in the other direction: a motor driven when nothing should be.
        commandRest(now);
        raise(AlarmKind::DeliveryFault, "the motor is driven with no therapy running", now);
    }
    if (state_ != State::Infusing) {
        return;
    }

    if (readback.pressureMmHg >= parameters_.occlusionMmHg) {
        raise(AlarmKind::Occlusion,
              "line pressure " + num(readback.pressureMmHg, 0) + " mmHg (limit " +
                  num(parameters_.occlusionMmHg, 0) + ")",
              now);
        return;
    }
    if (readback.air) {
        raise(AlarmKind::Air, "air in the line", now);
        return;
    }

    // INF-08. Stopped on the readback that reaches the volume, so the excess
    // is bounded by one readback period at the programmed rate (2.8 uL at
    // 100 mL/h and 10 Hz) - a bound, not zero. Zero needs the mechanism to
    // count the volume itself, which is the real-time core's job (phase 10).
    if (deliveredMl() >= program_.vtbiMl) {
        commandRest(now);
        deliveredBaseMl_ += mlFromMm(last_.encoderMm - stretchStartMm_);
        stretchRunning_ = false;
        endTherapy("completed");
        transition(State::Completed);
        raise(AlarmKind::VtbiComplete, "volume to be infused delivered", now);
        return;
    }

    // INF-05: measured flow against commanded, over whole windows.
    const auto elapsed = now - windowStart_;
    if (elapsed >= parameters_.deliveryWindow) {
        const double measured = mlFromMm(readback.encoderMm - windowStartMm_);
        const double expected = program_.rateMlH / 3600.0 * seconds(elapsed);
        if (expected > 0.0 &&
            std::fabs(measured - expected) > parameters_.deliveryTolerance * expected) {
            raise(AlarmKind::DeliveryFault,
                  "delivered " + num(measured * 1000.0, 1) + " uL where " +
                      num(expected * 1000.0, 1) + " uL were commanded",
                  now);
            return;
        }
        restartWindow(now);
    }
}

void Controller::tick(Clock::time_point now) {
    if (silencedUntil_ != Clock::time_point{} && now >= silencedUntil_) {
        silencedUntil_ = Clock::time_point{};
        for (Alarm& a : alarms_) {
            a.silenced = false;
            audit_.audit(med::AuditEvent::AlarmRaised, "silence expired; the alarm sounds again",
                         {{"alarm", toString(a.kind)}, {"priority", toString(priorityOf(a.kind))}});
        }
    }

    if (linkUp_ && now >= nextHeartbeat_) {
        nextHeartbeat_ = now + parameters_.heartbeatPeriod;
        if (actuator_.heartbeat() != med::Status::Ok) {
            linkUp_ = false;
            raise(AlarmKind::ProducerLost, "the mechanism did not answer: " + actuator_.lastError(), now);
        }
    }

    if (supervising_ && now - lastReadbackAt_ > parameters_.producerTimeout) {
        supervising_ = false;
        haveReadback_ = false;
        raise(AlarmKind::ProducerLost, "no readback from the mechanism", now);
    }
}

void Controller::therapyWasInterrupted(Clock::time_point now) {
    // The program is not restored: what the patient was receiving is in the
    // trail, and resuming it is an operator's decision made afresh.
    audit_.audit(med::AuditEvent::TherapyStopped, "therapy interrupted by a restart",
                 {{"reason", "interrupted"}});
    raise(AlarmKind::TherapyInterrupted, "the service restarted with a therapy unfinished", now);
}

void Controller::linkRestored(Clock::time_point now) {
    linkUp_ = true;
    nextHeartbeat_ = now;
}

std::string Controller::statusText() const {
    std::string alarms;
    for (const Alarm& a : alarms_) {
        alarms += (alarms.empty() ? "" : ",") + std::string(toString(a.kind)) + ":" +
                  toString(priorityOf(a.kind));
    }
    std::string out;
    out += "state=" + std::string(toString(state_)) + "\n";
    out += "drug=" + program_.drug + "\n";
    out += "syringe=" + program_.syringe + "\n";
    out += "rate_ml_h=" + num(program_.rateMlH) + "\n";
    out += "vtbi_ml=" + num(program_.vtbiMl) + "\n";
    out += "delivered_ml=" + num(deliveredMl(), 4) + "\n";
    out += "pressure_mmhg=" + num(last_.pressureMmHg, 1) + "\n";
    out += "alarms=" + (alarms.empty() ? std::string("none") : alarms) + "\n";
    out += "silenced=" + std::string(silenced() ? "1" : "0") + "\n";
    out += "therapy_active=" + std::string(therapyActive_ ? "1" : "0") + "\n";
    out += "mechanism=" + std::string(linkUp_ && haveReadback_ ? "up" : "down") + "\n";
    return out;
}

}  // namespace infusion
