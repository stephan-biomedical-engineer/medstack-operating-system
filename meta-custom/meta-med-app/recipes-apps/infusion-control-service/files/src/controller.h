// SPDX-License-Identifier: MIT
//
// infusion-control-service - the controller: therapy state, dose error
// reduction, alarms and the supervision of the mechanism.
//
// Pure: no socket, no clock, no journal. Time comes in as an argument, the
// mechanism is an ActuatorPort and the trail is an AuditPort, so a test can walk
// every transition - the forbidden ones included - in microseconds, and the
// service wires the same object to the framework's drivers.
//
//   idle -> programmed -> infusing <-> paused -> completed
//                             \           |
//                              `--> alarm <'      (and from idle, if the
//                                                  mechanism is lost)
//
// The rules that make it a controller for something that ACTS on a patient,
// each traced to docs/REQUISITOS_INFUSAO.md:
//
//   * a rate above the drug's hard limit is refused, above its soft limit it
//     needs an operator's confirmation, and both are audited (INF-01, INF-02);
//   * delivered volume is the encoder's, never the command's (INF-04), and a
//     measured flow off the commanded one by more than the tolerance over a
//     whole window is a delivery fault - in both directions, and a motor
//     driven while no therapy runs is one too (INF-05);
//   * occlusion, air, delivery fault, a mechanism that tripped its own dead-man
//     timer or stopped answering: high priority, motor at rest at once
//     (INF-06, INF-07, INF-09, INF-10);
//   * the end of the volume to be infused stops the motor and alarms at medium
//     priority, and nothing is delivered past it (INF-08);
//   * an alarm is cleared only by an operator, and only once its cause is gone;
//     after it the therapy is PAUSED, never resumed: resuming is a decision
//     (INF-11, INF-12). Silence lasts at most silenceMax and an alarm whose
//     cause persists sounds again;
//   * every transition, alarm, acknowledgement and override is an audit record
//     (INF-13).

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <medplatform/MedicalLogger.h>
#include <medplatform/MedicalTypes.h>

namespace infusion {

using Clock = std::chrono::steady_clock;

struct Drug {
    std::string id;
    std::string name;
    double softMaxMlH = 0.0;
    double hardMaxMlH = 0.0;
};

struct SyringeSpec {
    std::string id;
    double volumeMl = 0.0;
    double areaMm2 = 0.0;
};

struct Library {
    std::vector<Drug> drugs;
    std::vector<SyringeSpec> syringes;
    const Drug* drug(const std::string& id) const;
    const SyringeSpec* syringe(const std::string& id) const;
};

struct Parameters {
    std::chrono::milliseconds heartbeatPeriod{200};
    std::chrono::milliseconds producerTimeout{600};
    double occlusionMmHg = 500.0;
    double deliveryTolerance = 0.05;
    std::chrono::milliseconds deliveryWindow{2000};
    std::chrono::milliseconds silenceMax{120000};
    double maxRateMlH = 1200.0;
    /// After the motor is told to rest, readbacks already on their way may
    /// still show it driven; for this long they are not a delivery fault.
    std::chrono::milliseconds restSettle{500};
};

enum class State { Idle, Programmed, Infusing, Paused, Completed, Alarm };
enum class Priority { Low, Medium, High };
enum class AlarmKind {
    Occlusion,
    Air,
    DeliveryFault,
    VtbiComplete,
    ProducerLost,
    ActuatorFault,
    TherapyInterrupted,
};

const char* toString(State state) noexcept;
const char* toString(Priority priority) noexcept;
const char* toString(AlarmKind kind) noexcept;
Priority priorityOf(AlarmKind kind) noexcept;

/// One readback from the mechanism, in its units.
struct Readback {
    double encoderMm = 0.0;
    double pressureMmHg = 0.0;
    bool air = false;
    bool motor = false;
    bool tripped = false;
};

/// The mechanism, as the controller drives it. The service implements it over
/// MedicalActuator; a test, over a recorder.
class ActuatorPort {
public:
    virtual ~ActuatorPort() = default;
    virtual med::Status setRate(double mlPerHour) = 0;
    virtual med::Status heartbeat() = 0;
    virtual med::Status safeState() = 0;
    virtual std::string lastError() const = 0;
};

class AuditPort {
public:
    virtual ~AuditPort() = default;
    virtual void audit(med::AuditEvent event, const std::string& message,
                       const std::vector<med::LogField>& fields) = 0;
};

struct Program {
    std::string drug;
    std::string syringe;
    double rateMlH = 0.0;
    double vtbiMl = 0.0;
    /// Who confirmed a rate above the soft limit; empty when none was needed.
    std::string overriddenBy;
};

struct Reply {
    bool ok = false;
    std::string detail;
};

struct Alarm {
    AlarmKind kind;
    std::string detail;
    bool silenced = false;
};

class Controller {
public:
    Controller(Library library, Parameters parameters, ActuatorPort& actuator, AuditPort& audit);

    /// `confirmSoft` with a non-empty `operatorName` accepts a rate above the
    /// drug's soft limit (and below its hard limit), on that operator's word.
    Reply program(const std::string& drug, const std::string& syringe, double rateMlH,
                  double vtbiMl, bool confirmSoft, const std::string& operatorName);
    Reply start(Clock::time_point now);
    Reply pause(Clock::time_point now);
    Reply resume(Clock::time_point now);
    Reply stop(Clock::time_point now);
    Reply acknowledge(const std::string& operatorName, Clock::time_point now);
    Reply silence(const std::string& operatorName, Clock::time_point now);

    void onReadback(const Readback& readback, Clock::time_point now);
    /// Heartbeats, the mechanism's silence, silence expiry. Call often.
    void tick(Clock::time_point now);

    /// At start-up, when the previous run left a therapy unfinished (INF-11).
    void therapyWasInterrupted(Clock::time_point now);
    /// The service re-established the mechanism's session (after a lost or
    /// tripped one); the controller may clear those alarms on acknowledgement.
    void linkRestored(Clock::time_point now);
    bool linkUp() const { return linkUp_; }
    bool needsReconnect() const;

    State state() const { return state_; }
    /// A therapy was started and has not ended (stopped or completed). This is
    /// what the service persists, so that a restart finds it unfinished.
    bool therapyActive() const { return therapyActive_; }
    double deliveredMl() const;
    const Program& currentProgram() const { return program_; }
    const std::vector<Alarm>& alarms() const { return alarms_; }
    bool silenced() const { return silencedUntil_ != Clock::time_point{}; }

    /// key=value lines for the status file the command line and the
    /// acceptance suite read.
    std::string statusText() const;

private:
    Reply refuse(const std::string& detail) const { return Reply{false, detail}; }
    Reply refuseIn(const char* verb) const;
    void raise(AlarmKind kind, const std::string& detail, Clock::time_point now);
    bool active(AlarmKind kind) const;
    bool causePersists(const Alarm& alarm) const;
    void toRest(const std::string& why, Clock::time_point now);
    void commandRest(Clock::time_point now);
    void endTherapy(const std::string& reason);
    void restartWindow(Clock::time_point now);
    void transition(State next);
    double mlFromMm(double mm) const;

    Library library_;
    Parameters parameters_;
    ActuatorPort& actuator_;
    AuditPort& audit_;

    State state_ = State::Idle;
    Program program_;
    const SyringeSpec* syringe_ = nullptr;
    bool therapyActive_ = false;

    bool haveReadback_ = false;
    Readback last_;
    Clock::time_point lastReadbackAt_{};
    bool supervising_ = false;

    /// Delivered before the current running stretch, and where it began.
    double deliveredBaseMl_ = 0.0;
    double stretchStartMm_ = 0.0;
    bool stretchRunning_ = false;

    Clock::time_point windowStart_{};
    double windowStartMm_ = 0.0;

    Clock::time_point restSince_{};
    bool linkUp_ = true;
    Clock::time_point nextHeartbeat_{};

    std::vector<Alarm> alarms_;
    Clock::time_point silencedUntil_{};
};

}  // namespace infusion
