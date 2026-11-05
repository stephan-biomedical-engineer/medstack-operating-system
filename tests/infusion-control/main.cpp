// SPDX-License-Identifier: MIT
//
// infusion-control-service - host tests (implementation_plan_infusion_pump.md,
// phase 4). The controller is driven through a recording actuator and a
// recording audit trail, with time passed in, so every transition - the
// forbidden ones above all - is checked in microseconds, and so is every alarm.
//
// What none of it says: anything about a real pump. Every number is the
// model's (docs/REQUISITOS_INFUSAO.md section 2).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <medplatform/MedicalConfiguration.h>

#include "check.h"
#include "controller.h"
#include "library.h"

using namespace infusion;
using std::chrono::milliseconds;

namespace {

struct FakeActuator : ActuatorPort {
    std::vector<double> rates;
    int heartbeats = 0;
    int rests = 0;
    med::Status setStatus = med::Status::Ok;
    med::Status heartbeatStatus = med::Status::Ok;
    med::Status setRate(double r) override {
        rates.push_back(r);
        return setStatus;
    }
    med::Status heartbeat() override {
        ++heartbeats;
        return heartbeatStatus;
    }
    med::Status safeState() override {
        ++rests;
        return med::Status::Ok;
    }
    std::string lastError() const override { return "fake"; }
};

struct Record {
    med::AuditEvent event;
    std::string message;
    std::map<std::string, std::string> fields;
};

struct FakeAudit : AuditPort {
    std::vector<Record> records;
    void audit(med::AuditEvent event, const std::string& message,
               const std::vector<med::LogField>& fields) override {
        Record r{event, message, {}};
        for (const auto& f : fields) {
            r.fields[f.key] = f.value;
        }
        records.push_back(r);
    }
    int count(med::AuditEvent e) const {
        int n = 0;
        for (const Record& r : records) {
            n += r.event == e ? 1 : 0;
        }
        return n;
    }
    /// The field of the last record of `e`, empty when there is none: a test
    /// that crashes on a missing record hides every check after it.
    std::string field(med::AuditEvent e, const std::string& key) const {
        const Record* r = last(e);
        if (r == nullptr || r->fields.count(key) == 0) {
            return {};
        }
        return r->fields.at(key);
    }
    std::string message(med::AuditEvent e) const {
        const Record* r = last(e);
        return r == nullptr ? std::string() : r->message;
    }
    const Record* last(med::AuditEvent e) const {
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            if (it->event == e) {
                return &*it;
            }
        }
        return nullptr;
    }
};

Library library() {
    Library l;
    l.drugs = {{"saline", "Saline 0.9%", 500.0, 1000.0}, {"heparin", "Heparin", 30.0, 50.0}};
    l.syringes = {{"50ml", 50.0, 555.72}};
    return l;
}

const double kArea = 555.72;

/// The mechanism, as the test plays it: an encoder that advances at the rate
/// it was last given, read back every 100 ms.
struct Bench {
    FakeActuator actuator;
    FakeAudit audit;
    Controller controller{library(), Parameters{}, actuator, audit};
    Clock::time_point t0 = Clock::now();
    long long ms = 0;
    double encoderMm = 0.0;
    double pressure = 20.0;
    bool air = false, tripped = false;
    /// Measured flow as a fraction of the commanded one.
    double flowFactor = 1.0;
    bool motorOverride = false;

    Clock::time_point now() const { return t0 + milliseconds(ms); }
    double commanded() const {
        const bool running = controller.state() == State::Infusing;
        return running && !actuator.rates.empty() ? actuator.rates.back() : 0.0;
    }
    void feed() {
        Readback r;
        r.encoderMm = encoderMm;
        r.pressureMmHg = pressure;
        r.air = air;
        r.motor = motorOverride || commanded() > 0.0;
        r.tripped = tripped;
        controller.onReadback(r, now());
    }
    /// Advance `duration` ms in 100 ms readbacks, ticking every 50 ms.
    void run(long long duration) {
        for (long long step = 0; step < duration; step += 50) {
            const double mlPerS = commanded() / 3600.0 * flowFactor;
            encoderMm += mlPerS * 0.05 * 1000.0 / kArea;
            ms += 50;
            controller.tick(now());
            if (ms % 100 == 0) {
                feed();
            }
        }
    }
};

/// Each state the controller can be in, reached the way an operator reaches it.
void toProgrammed(Bench& b) {
    b.feed();
    b.controller.program("saline", "50ml", 100.0, 10.0, false, "");
}
void toInfusing(Bench& b) {
    toProgrammed(b);
    b.controller.start(b.now());
    b.run(500);
}
void toPaused(Bench& b) {
    toInfusing(b);
    b.controller.pause(b.now());
}
void toCompleted(Bench& b) {
    b.feed();
    b.controller.program("saline", "50ml", 400.0, 0.2, false, "");
    b.controller.start(b.now());
    b.run(2500);
}
void toCompletedAcked(Bench& b) {
    toCompleted(b);
    b.controller.acknowledge("op", b.now());
}
void toAlarm(Bench& b) {
    toInfusing(b);
    b.pressure = 600.0;
    b.run(200);
}

void testTransitions() {
    medtest::section("Controlador - toda transição, as proibidas incluídas");

    struct Case {
        const char* name;
        std::function<void(Bench&)> reach;
        State expected;
        // program, start, pause, resume, stop, acknowledge, silence
        bool allowed[7];
    };
    const Case cases[] = {
        {"idle", [](Bench& b) { b.feed(); }, State::Idle, {true, false, false, false, false, false, false}},
        {"programmed", toProgrammed, State::Programmed, {true, true, false, false, true, false, false}},
        {"infusing", toInfusing, State::Infusing, {false, false, true, false, true, false, false}},
        {"paused", toPaused, State::Paused, {false, false, false, true, true, false, false}},
        {"completed, alarm active", toCompleted, State::Completed, {false, false, false, false, true, true, true}},
        {"completed, acknowledged", toCompletedAcked, State::Completed, {true, false, false, false, true, false, false}},
        // acknowledge is refused while the occlusion persists.
        {"alarm", toAlarm, State::Alarm, {false, false, false, false, true, false, true}},
    };
    const char* verbs[] = {"program", "start", "pause", "resume", "stop", "acknowledge", "silence"};

    int checked = 0;
    for (const Case& c : cases) {
        for (int v = 0; v < 7; ++v) {
            Bench b;
            c.reach(b);
            CHECK_MSG(b.controller.state() == c.expected,
                      std::string(c.name) + ": chegou em " + toString(b.controller.state()));
            const std::size_t ratesBefore = b.actuator.rates.size();
            Reply r;
            switch (v) {
                case 0: r = b.controller.program("saline", "50ml", 50.0, 5.0, false, ""); break;
                case 1: r = b.controller.start(b.now()); break;
                case 2: r = b.controller.pause(b.now()); break;
                case 3: r = b.controller.resume(b.now()); break;
                case 4: r = b.controller.stop(b.now()); break;
                case 5: r = b.controller.acknowledge("op", b.now()); break;
                case 6: r = b.controller.silence("op", b.now()); break;
            }
            CHECK_MSG(r.ok == c.allowed[v], std::string(c.name) + " + " + verbs[v] + ": " +
                                                (r.ok ? "aceito" : "recusado") + " (" + r.detail + ")");
            if (!r.ok && v != 5) {
                // A refused verb commands nothing.
                CHECK_MSG(b.actuator.rates.size() == ratesBefore,
                          std::string(c.name) + " + " + verbs[v] + " recusado e mesmo assim comandou");
            }
            ++checked;
        }
    }
    CHECK_EQ(checked, 49);
}

void testDers() {
    medtest::section("Controlador - DERS (INF-01, INF-02)");
    {
        Bench b;
        b.feed();
        Reply r = b.controller.program("heparin", "50ml", 60.0, 5.0, true, "ana");
        CHECK(!r.ok);
        CHECK(r.detail.find("hard limit") != std::string::npos);
        CHECK_EQ(b.audit.count(med::AuditEvent::SafetyLimitViolation), 1);
        CHECK(b.audit.field(med::AuditEvent::SafetyLimitViolation, "drug") == "heparin");
        CHECK(b.controller.state() == State::Idle);

        // Above soft: refused until confirmed, by someone.
        r = b.controller.program("heparin", "50ml", 40.0, 5.0, false, "");
        CHECK(!r.ok && r.detail.find("confirm") != std::string::npos);
        r = b.controller.program("heparin", "50ml", 40.0, 5.0, true, "");
        CHECK(!r.ok && r.detail.find("named operator") != std::string::npos);
        CHECK_EQ(b.audit.count(med::AuditEvent::SafetyLimitOverridden), 0);
        r = b.controller.program("heparin", "50ml", 40.0, 5.0, true, "ana");
        CHECK(r.ok);
        CHECK_EQ(b.audit.count(med::AuditEvent::SafetyLimitOverridden), 1);
        CHECK(b.audit.field(med::AuditEvent::SafetyLimitOverridden, "operator") == "ana");
        CHECK(b.controller.currentProgram().overriddenBy == "ana");

        // Within soft: no override recorded.
        r = b.controller.program("heparin", "50ml", 20.0, 5.0, false, "");
        CHECK(r.ok && b.controller.currentProgram().overriddenBy.empty());
        CHECK_EQ(b.audit.count(med::AuditEvent::SafetyLimitOverridden), 1);

        CHECK(!b.controller.program("morphine", "50ml", 1.0, 1.0, false, "").ok);
        CHECK(!b.controller.program("saline", "60ml", 1.0, 1.0, false, "").ok);
        CHECK(!b.controller.program("saline", "50ml", 0.0, 1.0, false, "").ok);
        CHECK(!b.controller.program("saline", "50ml", NAN, 1.0, false, "").ok);
        CHECK(!b.controller.program("saline", "50ml", 1300.0, 1.0, true, "ana").ok);
        CHECK(!b.controller.program("saline", "50ml", 10.0, 51.0, false, "").ok);
        CHECK(!b.controller.program("saline", "50ml", 10.0, 0.0, false, "").ok);
    }
}

void testDelivery() {
    medtest::section("Controlador - volume e fim do VTBI (INF-04, INF-08)");
    {
        Bench b;
        b.feed();
        CHECK(b.controller.program("saline", "50ml", 100.0, 1.0, false, "").ok);
        CHECK(b.controller.start(b.now()).ok);
        CHECK(b.controller.therapyActive());
        b.run(30000);
        CHECK(b.controller.state() == State::Infusing);
        CHECK_NEAR(b.controller.deliveredMl(), 100.0 / 3600.0 * 30.0, 0.002);
        b.run(7000);
        CHECK(b.controller.state() == State::Completed);
        CHECK(!b.controller.therapyActive());
        CHECK(b.controller.deliveredMl() >= 1.0);
        // Bounded by one readback period at the rate: 2.8 uL.
        CHECK(b.controller.deliveredMl() <= 1.0 + 100.0 / 3600.0 * 0.1 + 1e-9);
        CHECK(!b.controller.alarms().empty() &&
              b.controller.alarms()[0].kind == AlarmKind::VtbiComplete);
        CHECK(priorityOf(AlarmKind::VtbiComplete) == Priority::Medium);
        CHECK(b.actuator.rests >= 1);
        CHECK(b.audit.field(med::AuditEvent::TherapyStopped, "reason") == "completed");
        std::printf("   VTBI 1 mL a 100 mL/h: entregue %.4f mL\n", b.controller.deliveredMl());
    }
    {
        // A pause does not lose the volume delivered before it.
        Bench b;
        toInfusing(b);
        b.run(9500);
        const double before = b.controller.deliveredMl();
        b.controller.pause(b.now());
        b.run(5000);
        CHECK_NEAR(b.controller.deliveredMl(), before, 1e-9);
        b.controller.resume(b.now());
        b.run(10000);
        CHECK_NEAR(b.controller.deliveredMl(), 100.0 / 3600.0 * 20.0, 0.002);
    }
}

void testAlarms() {
    medtest::section("Controlador - oclusão, ar, sem retomada automática (INF-06, INF-07, INF-11)");
    {
        Bench b;
        toInfusing(b);
        const std::size_t rates = b.actuator.rates.size();
        b.pressure = 600.0;
        b.run(100);
        CHECK(b.controller.state() == State::Alarm);
        CHECK(b.actuator.rests >= 1);
        CHECK(b.audit.field(med::AuditEvent::AlarmRaised, "alarm") == "occlusion");
        CHECK(b.audit.field(med::AuditEvent::AlarmRaised, "priority") == "high");
        CHECK(b.audit.field(med::AuditEvent::TherapyStopped, "reason") == "alarm:occlusion");
        CHECK(b.controller.therapyActive());

        // The cause persists: acknowledged, not cleared.
        Reply r = b.controller.acknowledge("op", b.now());
        CHECK(!r.ok && r.detail.find("persists") != std::string::npos);
        CHECK(b.controller.state() == State::Alarm);

        b.pressure = 15.0;
        b.run(200);
        r = b.controller.acknowledge("op", b.now());
        CHECK(r.ok);
        // Paused, never infusing: resuming is the operator's decision.
        CHECK(b.controller.state() == State::Paused);
        CHECK(b.actuator.rates.size() == rates);
        CHECK(b.controller.resume(b.now()).ok);
        CHECK(b.controller.state() == State::Infusing);
        CHECK_EQ(b.audit.count(med::AuditEvent::AlarmAcknowledged), 2);
    }
    {
        Bench b;
        toInfusing(b);
        b.air = true;
        b.run(100);
        CHECK(b.controller.state() == State::Alarm);
        CHECK(b.controller.alarms()[0].kind == AlarmKind::Air);
    }
    {
        // A restart with a therapy unfinished: alarm, and nothing commanded.
        Bench b;
        b.feed();
        b.controller.therapyWasInterrupted(b.now());
        CHECK(b.controller.state() == State::Alarm);
        CHECK(b.controller.alarms()[0].kind == AlarmKind::TherapyInterrupted);
        CHECK(b.actuator.rates.empty());
        CHECK(b.audit.field(med::AuditEvent::TherapyStopped, "reason") == "interrupted");
        CHECK(b.controller.acknowledge("op", b.now()).ok);
        CHECK(b.controller.state() == State::Idle);
        CHECK(b.controller.currentProgram().drug.empty());
    }

    medtest::section("Controlador - falha de entrega nos dois sentidos (INF-05)");
    {
        // Within tolerance: 1 % off for 20 s, no fault.
        Bench b;
        toInfusing(b);
        b.flowFactor = 0.99;
        b.run(20000);
        CHECK(b.controller.state() == State::Infusing);
    }
    for (double factor : {0.0, 0.9, 1.2, 2.0}) {
        // Jammed, slow, fast, runaway: a fault within one window and a frame.
        Bench b;
        toInfusing(b);
        b.flowFactor = factor;
        b.run(2200);
        CHECK_MSG(b.controller.state() == State::Alarm,
                  "fluxo " + std::to_string(factor) + "x não gerou falha de entrega");
        CHECK(!b.controller.alarms().empty() &&
              b.controller.alarms()[0].kind == AlarmKind::DeliveryFault);
    }
    {
        // A motor driven with no therapy running - after the settling time.
        Bench b;
        toPaused(b);
        b.motorOverride = true;
        b.run(300);
        CHECK(b.controller.state() == State::Paused);  // still settling
        b.run(500);
        CHECK(b.controller.state() == State::Alarm);
        CHECK(b.controller.alarms()[0].kind == AlarmKind::DeliveryFault);
    }

    medtest::section("Controlador - o mecanismo supervisionado (INF-09, INF-10)");
    {
        // Heartbeats at the period: 1 s of ticks every 50 ms, 5 or 6 beats.
        Bench b;
        b.feed();
        const int before = b.actuator.heartbeats;
        b.run(1000);
        CHECK(b.actuator.heartbeats - before >= 5 && b.actuator.heartbeats - before <= 6);
    }
    {
        // A heartbeat that fails: the mechanism is lost; motor at rest; the
        // alarm stays until the link is back.
        Bench b;
        toInfusing(b);
        b.actuator.heartbeatStatus = med::Status::Timeout;
        b.run(300);
        CHECK(b.controller.state() == State::Alarm);
        CHECK(!b.controller.linkUp());
        CHECK(b.controller.needsReconnect());
        CHECK_EQ(b.audit.count(med::AuditEvent::ActuatorFault), 1);
        CHECK(!b.controller.acknowledge("op", b.now()).ok);
        b.actuator.heartbeatStatus = med::Status::Ok;
        b.controller.linkRestored(b.now());
        b.run(200);
        CHECK(b.controller.acknowledge("op", b.now()).ok);
        CHECK(b.controller.state() == State::Paused);
    }
    {
        // Silence of the readback past the timeout.
        Bench b;
        toInfusing(b);
        for (int i = 0; i < 20; ++i) {
            b.ms += 50;
            b.controller.tick(b.now());  // no readback
        }
        CHECK(b.controller.state() == State::Alarm);
        CHECK(b.controller.alarms()[0].kind == AlarmKind::ProducerLost);
    }
    {
        // The mechanism's own dead-man trip, seen in the readback.
        Bench b;
        toInfusing(b);
        b.tripped = true;
        b.run(100);
        CHECK(b.controller.state() == State::Alarm);
        CHECK(b.controller.alarms()[0].kind == AlarmKind::ActuatorFault);
    }

    medtest::section("Controlador - silêncio limitado (INF-12)");
    {
        Bench b;
        toAlarm(b);
        CHECK(!b.controller.silence("", b.now()).ok);
        CHECK(b.controller.silence("op", b.now()).ok);
        CHECK(b.controller.silenced());
        b.run(119000);
        CHECK(b.controller.silenced());
        const int raised = b.audit.count(med::AuditEvent::AlarmRaised);
        b.run(2000);
        CHECK(!b.controller.silenced());
        CHECK(!b.controller.alarms()[0].silenced);
        CHECK(b.audit.count(med::AuditEvent::AlarmRaised) == raised + 1);
        CHECK(b.audit.message(med::AuditEvent::AlarmRaised).find("silence expired") !=
              std::string::npos);
    }
}

void testTrail() {
    medtest::section("Controlador - a trilha de uma terapia inteira (INF-13)");
    Bench b;
    b.feed();
    b.controller.program("saline", "50ml", 400.0, 0.2, false, "");
    b.controller.start(b.now());
    b.run(500);
    b.controller.pause(b.now());
    b.controller.resume(b.now());
    b.run(2000);
    b.controller.acknowledge("op", b.now());
    using E = med::AuditEvent;
    CHECK_EQ(b.audit.count(E::ConfigurationChanged), 1);
    CHECK_EQ(b.audit.count(E::TherapyStarted), 2);   // start, resume
    CHECK_EQ(b.audit.count(E::TherapyStopped), 2);   // paused, completed
    CHECK_EQ(b.audit.count(E::AlarmRaised), 1);      // vtbi_complete
    CHECK_EQ(b.audit.count(E::AlarmAcknowledged), 1);
    CHECK(b.audit.records.size() == 7);
}

void put(med::MedicalConfiguration& c, const std::map<std::string, std::string>& values) {
    for (const auto& v : values) {
        c.set(v.first, v.second);
    }
}

std::map<std::string, std::string> goodConfig() {
    return {
        {"service.heartbeat_ms", "200"}, {"service.missed_heartbeats", "3"},
        {"service.producer_timeout_ms", "600"}, {"alarm.occlusion_mmhg", "500"},
        {"alarm.delivery_tolerance", "0.05"}, {"alarm.delivery_window_ms", "2000"},
        {"alarm.silence_max_s", "120"}, {"mechanism.max_rate_ml_h", "1200"},
        {"mechanism.frame_rate_hz", "10"}, {"mechanism.command", "/run/a.sock"},
        {"mechanism.data", "/run/b.sock"}, {"control.socket", "/run/c.sock"},
        {"control.status", "/run/status"}, {"control.therapy_marker", "/var/lib/m"},
        {"syringes", "50ml"}, {"syringe.50ml.volume_ml", "50"}, {"syringe.50ml.area_mm2", "555.72"},
        {"drugs", "saline, heparin"}, {"drug.saline.name", "Saline"},
        {"drug.saline.soft_max_ml_h", "500"}, {"drug.saline.hard_max_ml_h", "1000"},
        {"drug.heparin.name", "Heparin"}, {"drug.heparin.soft_max_ml_h", "30"},
        {"drug.heparin.hard_max_ml_h", "50"},
    };
}

void testConfiguration() {
    medtest::section("Configuração selada - recusas (INF-03, INF-16)");
    {
        auto c = med::MedicalConfiguration::empty();
        put(*c, goodConfig());
        std::vector<std::string> problems;
        auto loaded = loadServiceConfig(*c, &problems);
        CHECK_MSG(loaded.isOk(), problems.empty() ? "" : problems.front());
        CHECK_EQ(static_cast<long long>(loaded.value().library.drugs.size()), 2LL);
        CHECK(loaded.value().library.drug("heparin")->hardMaxMlH == 50.0);
        CHECK_EQ(static_cast<long long>(loaded.value().parameters.heartbeatPeriod.count()), 200LL);
    }
    struct Bad {
        const char* what;
        std::map<std::string, std::string> change;
        const char* expect;
    };
    const Bad bad[] = {
        {"soft above hard", {{"drug.heparin.soft_max_ml_h", "60"}}, "above the hard limit"},
        {"hard above the mechanism", {{"drug.saline.hard_max_ml_h", "5000"}}, "hard_max"},
        {"dead-man window too long", {{"service.heartbeat_ms", "1000"}, {"service.missed_heartbeats", "3"}}, "dead-man"},
        {"occlusion limit out of range", {{"alarm.occlusion_mmhg", "1000"}}, "occlusion"},
        {"fractional heartbeats", {{"service.missed_heartbeats", "2.5"}}, "missed_heartbeats"},
        {"no drugs", {{"drugs", ""}}, "no drug"},
        {"bad drug id", {{"drugs", "Saline!"}}, "valid"},
        {"duplicate drug", {{"drugs", "saline,saline"}}, "unique"},
        {"producer timeout under two frames", {{"service.producer_timeout_ms", "150"}}, "two readback"},
        {"drug without a name", {{"drug.saline.name", ""}}, "name"},
    };
    for (const Bad& b : bad) {
        auto c = med::MedicalConfiguration::empty();
        std::map<std::string, std::string> values = goodConfig();
        for (const auto& kv : b.change) {
            values[kv.first] = kv.second;
        }
        put(*c, values);
        std::vector<std::string> problems;
        auto loaded = loadServiceConfig(*c, &problems);
        bool named = false;
        for (const std::string& p : problems) {
            named = named || p.find(b.expect) != std::string::npos;
        }
        CHECK_MSG(!loaded.isOk() && named, std::string(b.what) + ": " +
                                               (problems.empty() ? "aceito" : problems.front()));
    }
    {
        // The file the recipe ships passes the service's own judgement - so a
        // bad limit in it fails here, not as a service refusing to start on
        // the device.
        auto shipped = med::MedicalConfiguration::load(INFUSION_CONF, false);
        CHECK_MSG(shipped.isOk(), shipped.message());
        if (shipped) {
            std::vector<std::string> problems;
            auto loaded = loadServiceConfig(*shipped.value(), &problems);
            CHECK_MSG(loaded.isOk(), problems.empty() ? "" : problems.front());
            CHECK_EQ(static_cast<long long>(loaded.value().library.drugs.size()), 3LL);
            CHECK_EQ(static_cast<long long>(loaded.value().library.syringes.size()), 3LL);
            // The syringe the simulator is started with, at the area it models.
            CHECK(loaded.value().library.syringe("50ml") != nullptr &&
                  loaded.value().library.syringe("50ml")->areaMm2 == 555.72);
        }
    }
    {
        // Every problem is reported, not the first.
        auto c = med::MedicalConfiguration::empty();
        std::map<std::string, std::string> values = goodConfig();
        values["alarm.occlusion_mmhg"] = "1000";
        values["drug.heparin.soft_max_ml_h"] = "60";
        put(*c, values);
        std::vector<std::string> problems;
        CHECK(!loadServiceConfig(*c, &problems).isOk());
        CHECK(problems.size() >= 2);
    }
}

}  // namespace

int main() {
    std::printf("infusion-control-service - verificações de host\n");
    testTransitions();
    testDers();
    testDelivery();
    testAlarms();
    testTrail();
    testConfiguration();
    const int status = medtest::report();
    std::printf("\nO que estas verificações NÃO dizem: nada sobre uma bomba real, e nada sobre o\n"
                "serviço montado sobre os drivers - isso é a suíte do make check (Fase 7).\n");
    return status;
}
