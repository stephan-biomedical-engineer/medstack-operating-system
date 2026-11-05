// SPDX-License-Identifier: MIT
//
// MedicalActuator: the factory's refusals, the dead-man window a producer has
// to accept before anything moves, and the rule that an unanswered exchange
// ends the session - against a producer on a real socket (fake_producer.h).
//
// What this suite cannot see is the dead-man timer itself: it lives on the
// producer's side by design (MedicalActuator.h), so it is tested where it is
// implemented - the simulated mechanism's own suite, phase 3 of
// implementation_plan_infusion_pump.md. What is tested here is the half the
// framework owns: that the window is stated, required and honoured by the
// controller's side of the protocol.

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "MedicalActuator.h"
#include "check.h"
#include "fake_producer.h"
#include "temp_dir.h"

using namespace med;

namespace {

ActuatorConfig actuatorConfig(const std::string& path) {
    ActuatorConfig config;
    config.driver = "seqpacket";
    config.id = "mechanism0";
    config.address = path;
    config.heartbeatPeriod = std::chrono::milliseconds(200);
    config.missedHeartbeats = 3;
    return config;
}

std::unique_ptr<MedicalActuator> make(const ActuatorConfig& config) {
    Result<std::unique_ptr<MedicalActuator>> created = MedicalActuatorFactory::create(config);
    return created ? created.take() : nullptr;
}

}  // namespace

void testActuator() {
    medtest::section("MedicalActuator - fábrica e recusas");

    {
        const std::vector<std::string> drivers = MedicalActuatorFactory::drivers();
        CHECK_EQ(static_cast<long long>(drivers.size()), 1LL);
        CHECK(std::find(drivers.begin(), drivers.end(), "seqpacket") != drivers.end());
        CHECK(!MedicalActuatorFactory::registerDriver(
            "seqpacket", [](const ActuatorConfig&) {
                return Result<std::unique_ptr<MedicalActuator>>::fail(Status::Internal);
            }));

        ActuatorConfig unknown = actuatorConfig("/run/x.sock");
        unknown.driver = "nonexistent";
        CHECK_STATUS(MedicalActuatorFactory::create(unknown).status(), Status::NotFound);

        ActuatorConfig noAddress = actuatorConfig("");
        CHECK_STATUS(MedicalActuatorFactory::create(noAddress).status(), Status::InvalidArgument);

        ActuatorConfig noPeriod = actuatorConfig("/run/x.sock");
        noPeriod.heartbeatPeriod = std::chrono::milliseconds(0);
        CHECK_STATUS(MedicalActuatorFactory::create(noPeriod).status(), Status::InvalidArgument);

        // A window of zero heartbeats is a producer with no timer.
        ActuatorConfig noMiss = actuatorConfig("/run/x.sock");
        noMiss.missedHeartbeats = 0;
        CHECK_STATUS(MedicalActuatorFactory::create(noMiss).status(), Status::InvalidArgument);

        // An option may not speak to the dead-man timer: an application that
        // could send deadman_ms could also disarm it.
        for (const char* reserved : {actuation::kDeadmanKey, actuation::kHeartbeatKey,
                                     actuation::kSafeStateKey}) {
            ActuatorConfig sneaky = actuatorConfig("/run/x.sock");
            sneaky.driverOptions[reserved] = "1";
            CHECK_MSG(MedicalActuatorFactory::create(sneaky).status() == Status::InvalidArgument,
                      std::string("chave reservada aceita como opção: ") + reserved);
        }

        // Seven options fit beside the window; eight do not, and are refused
        // here rather than at the first start().
        ActuatorConfig seven = actuatorConfig("/run/x.sock");
        for (int i = 0; i < 7; ++i) {
            seven.driverOptions["option_" + std::to_string(i)] = "1";
        }
        CHECK(MedicalActuatorFactory::create(seven).isOk());
        ActuatorConfig eight = seven;
        eight.driverOptions["option_7"] = "1";
        CHECK_STATUS(MedicalActuatorFactory::create(eight).status(), Status::InvalidArgument);

        std::unique_ptr<MedicalActuator> created = make(actuatorConfig("/run/x.sock"));
        CHECK(created != nullptr);
        if (created) {
            CHECK(created->info().driver == "seqpacket");
            CHECK_EQ(static_cast<long long>(created->info().deadmanWindow.count()), 600LL);
            CHECK(!created->isRunning());
            // Nothing is sent by an actuator that was never started.
            CHECK_STATUS(created->command({{"speed", "1"}}), Status::Unavailable);
            CHECK_STATUS(created->heartbeat(), Status::Unavailable);
        }
    }

    medtest::section("MedicalActuator - a janela do homem-morto é exigida");

    medtest::TempDir dir;
    CHECK(dir.valid());

    {
        // start() states the window and the options in ONE message; the
        // producer accepts them together.
        medtest::FakeProducer::Script script;
        medtest::FakeProducer producer(dir.file("ok.sock"), script);
        ActuatorConfig config = actuatorConfig(dir.file("ok.sock"));
        config.driverOptions = {{"profile", "test"}};
        std::unique_ptr<MedicalActuator> actuator = make(config);
        CHECK(actuator != nullptr);
        if (actuator) {
            CHECK_STATUS(actuator->start(), Status::Ok);
            CHECK(actuator->isRunning());

            CHECK_STATUS(actuator->heartbeat(), Status::Ok);
            CHECK_STATUS(actuator->command({{"speed", "12.5"}}), Status::Ok);
            CHECK_STATUS(actuator->heartbeat(), Status::Ok);

            // Refused before anything is sent.
            CHECK_STATUS(actuator->command({}), Status::InvalidArgument);
            CHECK_STATUS(actuator->command({{actuation::kSafeStateKey, "0"}}),
                         Status::InvalidArgument);
            CHECK(actuator->isRunning());

            CHECK_STATUS(actuator->stop(), Status::Ok);
            CHECK(!actuator->isRunning());
        }
        producer.stop();

        const std::vector<std::map<std::string, std::string>> seen = producer.controls();
        const std::vector<std::map<std::string, std::string>> expected = {
            {{"deadman_ms", "600"}, {"profile", "test"}},
            {{"heartbeat", "1"}},
            {{"speed", "12.5"}},
            {{"heartbeat", "2"}},
            {{"safe_state", "1"}},
        };
        CHECK_EQ(static_cast<long long>(seen.size()), static_cast<long long>(expected.size()));
        CHECK_MSG(seen == expected, "o produtor não recebeu exatamente janela, sinais, comando e parada");
        CHECK(producer.clientClosed());
        CHECK_EQ(static_cast<long long>(producer.malformed()), 0LL);
    }

    {
        // A producer that does not know what a dead-man window is refuses the
        // key, as the control protocol obliges it to, and is never started.
        medtest::FakeProducer::Script script;
        script.refuseKey = actuation::kDeadmanKey;
        script.detail = "unknown setting";
        medtest::FakeProducer producer(dir.file("nodeadman.sock"), script);
        std::unique_ptr<MedicalActuator> actuator = make(actuatorConfig(dir.file("nodeadman.sock")));
        CHECK(actuator != nullptr);
        if (actuator) {
            CHECK_STATUS(actuator->start(), Status::InvalidArgument);
            CHECK(!actuator->isRunning());
            CHECK_MSG(actuator->lastError().find("deadman_ms") != std::string::npos &&
                          actuator->lastError().find("unknown setting") != std::string::npos,
                      actuator->lastError());
            CHECK_STATUS(actuator->command({{"speed", "1"}}), Status::Unavailable);
        }
    }

    {
        // A setpoint the producer refuses is an answer: the session goes on.
        medtest::FakeProducer::Script script;
        script.refuseKey = "speed";
        script.detail = "above the mechanism's limit";
        medtest::FakeProducer producer(dir.file("refuse.sock"), script);
        std::unique_ptr<MedicalActuator> actuator = make(actuatorConfig(dir.file("refuse.sock")));
        CHECK(actuator != nullptr);
        if (actuator) {
            CHECK_STATUS(actuator->start(), Status::Ok);
            CHECK_STATUS(actuator->command({{"speed", "9999"}}), Status::InvalidArgument);
            CHECK(actuator->lastError().find("above the mechanism's limit") != std::string::npos);
            CHECK(actuator->isRunning());
            CHECK_STATUS(actuator->heartbeat(), Status::Ok);
        }
    }

    medtest::section("MedicalActuator - uma troca sem resposta encerra a sessão");

    {
        // The producer answers the start and then goes quiet. The heartbeat
        // times out within one period, and from then on nothing is sent: a
        // late answer must never be read as the answer to the next request.
        medtest::FakeProducer::Script script;
        script.answerAtMost = 1;
        medtest::FakeProducer producer(dir.file("quiet.sock"), script);
        std::unique_ptr<MedicalActuator> actuator = make(actuatorConfig(dir.file("quiet.sock")));
        CHECK(actuator != nullptr);
        if (actuator) {
            CHECK_STATUS(actuator->start(), Status::Ok);
            const auto before = std::chrono::steady_clock::now();
            CHECK_STATUS(actuator->heartbeat(), Status::Timeout);
            const auto took = std::chrono::steady_clock::now() - before;
            CHECK(took >= std::chrono::milliseconds(190));
            CHECK(took < std::chrono::milliseconds(400));
            CHECK(!actuator->isRunning());

            CHECK_STATUS(actuator->command({{"speed", "1"}}), Status::Unavailable);
            CHECK_STATUS(actuator->heartbeat(), Status::Unavailable);
            CHECK_STATUS(actuator->safeState(), Status::Unavailable);
            // stop() closes without pretending to have brought it to rest; the
            // producer's own timer is what does that now.
            CHECK_STATUS(actuator->stop(), Status::Unavailable);
        }
        producer.stop();
        // The start, and the one heartbeat that went unanswered; nothing after.
        CHECK_EQ(static_cast<long long>(producer.controls().size()), 2LL);
    }

    {
        // Nobody listening: bounded, like a sensor's producer.
        std::unique_ptr<MedicalActuator> actuator = make(actuatorConfig(dir.file("absent.sock")));
        CHECK(actuator != nullptr);
        if (actuator) {
            const auto before = std::chrono::steady_clock::now();
            CHECK(actuator->start() != Status::Ok);
            const auto took = std::chrono::steady_clock::now() - before;
            CHECK(took >= std::chrono::milliseconds(4500));
            CHECK(took < std::chrono::milliseconds(7000));
            CHECK(!actuator->isRunning());
        }
    }
}
