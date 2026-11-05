// SPDX-License-Identifier: MIT
//
// infusion-sim - host tests (implementation_plan_infusion_pump.md, phase 3).
//
// Three levels, each able to fail without the others:
//   the model      - volume, pressure, occlusion time, stall, drift, end of travel
//   the mechanism  - the producer's side of the actuator protocol, with time
//                    passed in: above all the dead-man timer (INF-09)
//   the process    - the simulator behind real sockets, driven by the
//                    framework's own MedicalActuator and MedicalDevice drivers
//
// What none of it says: anything about a real pump. Every number checked here
// is the model's (docs/REQUISITOS_INFUSAO.md section 2).

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <medplatform/MedicalActuator.h>
#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>

#include "check.h"
#include "mechanism.h"
#include "simulator.h"
#include "syringe_model.h"
#include "temp_dir.h"

using namespace infusion;
using std::chrono::milliseconds;

namespace {

const Syringe& syringe(const char* name) { return *findSyringe(name); }

double encoderMl(const SyringeModel& model) {
    return model.encoderMm() * model.syringe().areaMm2 / 1000.0;
}

void testModel() {
    medtest::section("Modelo - seringas");
    CHECK(findSyringe("50ml") != nullptr && findSyringe("10ml") != nullptr &&
          findSyringe("20ml") != nullptr);
    CHECK(findSyringe("60ml") == nullptr);
    CHECK(findSyringe("") == nullptr);

    medtest::section("Modelo - volume entregue = integral da taxa (INF-04)");
    {
        SyringeModel model(syringe("50ml"));
        model.setRate(100.0);
        for (int i = 0; i < 6000; ++i) {
            model.step(0.01);  // 60 s
        }
        const double expected = 100.0 / 60.0;  // mL in one minute
        const double stepMl = 0.001 * model.syringe().areaMm2 / 1000.0;
        CHECK_NEAR(model.deliveredMl(), expected, stepMl);
        // The encoder agrees with the truth to one step: eps_vol is 1 % or one
        // step, the larger, and here one step is 0.03 %.
        CHECK_NEAR(encoderMl(model), expected, std::max(0.01 * expected, stepMl));
        CHECK(model.motorRunning());

        // A large dt is sub-stepped, so the answer does not depend on it.
        SyringeModel coarse(syringe("50ml"));
        coarse.setRate(100.0);
        coarse.step(60.0);
        CHECK_NEAR(coarse.deliveredMl(), expected, stepMl);
    }

    medtest::section("Modelo - pressão de linha");
    {
        // Resting pressure at 100 mL/h is Q x R = 20 mmHg.
        SyringeModel model(syringe("50ml"));
        model.setRate(100.0);
        model.step(10.0);
        CHECK_NEAR(model.pressureMmHg(), 20.0, 0.1);

        // Occluded: the pressure rises at Q/C, so 20 -> 500 mmHg takes
        // 480 x 0.001 / (100/3600) = 17.28 s. T_occ is 30 s.
        model.setOcclusion(true);
        double t = 0.0;
        while (model.pressureMmHg() < 500.0 && t < 60.0) {
            model.step(0.01);
            t += 0.01;
        }
        CHECK_NEAR(t, 17.28, 0.1);
        CHECK(t <= 30.0);
        std::printf("   oclusão a 100 mL/h: 500 mmHg em %.2f s (modelo)\n", t);

        // Unblocked and stopped, it decays with RC = 0.72 s.
        model.setOcclusion(false);
        model.setRate(0.0);
        model.step(10.0);
        CHECK(model.pressureMmHg() < 0.01);
    }

    medtest::section("Modelo - travamento, deriva, fim de curso");
    {
        // Jammed: the motor is driven and the encoder does not move - the
        // difference INF-05 detects.
        SyringeModel model(syringe("50ml"));
        model.setRate(100.0);
        model.step(5.0);
        const double before = model.encoderMm();
        model.setStall(true);
        model.step(10.0);
        CHECK(model.motorRunning());
        CHECK_NEAR(model.encoderMm(), before, 1e-12);
        CHECK(model.pressureMmHg() < 1.0);

        SyringeModel drifting(syringe("50ml"));
        drifting.setEncoderDrift(0.10);
        drifting.setRate(600.0);
        drifting.step(30.0);
        CHECK_NEAR(drifting.encoderMm(), 1.10 * drifting.plungerMm(), 0.002);

        // End of travel: never more than the syringe holds.
        SyringeModel small(syringe("10ml"));
        small.setRate(1200.0);
        small.step(40.0);  // 13.3 mL asked for
        CHECK(small.deliveredMl() <= 10.0 + 1e-9);
        CHECK(small.deliveredMl() > 10.0 - 0.001);
        CHECK(small.empty());
        CHECK(!small.motorRunning());

        SyringeModel negative(syringe("20ml"));
        negative.setRate(-5.0);
        CHECK_NEAR(negative.commandedRate(), 0.0, 0.0);
    }
}

using Clock = Mechanism::Clock;

void testMechanism() {
    const Clock::time_point t0 = Clock::now();
    auto at = [&](long long ms) { return t0 + milliseconds(ms); };

    medtest::section("Mecanismo - a janela vem antes de tudo");
    SyringeModel model(syringe("50ml"));
    Mechanism mech(model);
    {
        CHECK(mech.handle({{"deadman_ms", "600"}}, at(0)).rejected != 0);  // no session

        mech.connected(at(0));
        Mechanism::Answer a = mech.handle({{"rate_ml_h", "100"}}, at(1));
        CHECK_EQ(static_cast<int>(a.rejected), 1);
        CHECK(a.detail == "deadman_ms required first");
        CHECK(!mech.armed());
        CHECK_NEAR(model.commandedRate(), 0.0, 0.0);

        for (const char* bad : {"abc", "10", "100000", "600ms", " 600", ""}) {
            CHECK_MSG(mech.handle({{"deadman_ms", bad}}, at(2)).rejected == 1,
                      std::string("janela aceita: '") + bad + "'");
        }
        // The mechanism takes no option besides the window.
        CHECK_EQ(static_cast<int>(mech.handle({{"deadman_ms", "600"}, {"other", "1"}}, at(3)).rejected),
                 2);
        CHECK(!mech.armed());

        CHECK_EQ(static_cast<int>(mech.handle({{"deadman_ms", "600"}}, at(4)).rejected), 0);
        CHECK(mech.armed());
        CHECK_EQ(static_cast<long long>(mech.window().count()), 600LL);
        // Arming never starts it.
        CHECK_NEAR(model.commandedRate(), 0.0, 0.0);
        CHECK(mech.handle({{"deadman_ms", "600"}}, at(5)).detail == "already armed");
    }

    medtest::section("Mecanismo - setpoints, inteiros ou nada");
    {
        CHECK_EQ(static_cast<int>(mech.handle({{"rate_ml_h", "100"}}, at(10)).rejected), 0);
        CHECK_NEAR(model.commandedRate(), 100.0, 0.0);
        for (const char* bad : {"1300", "-1", "1e2x", "nan", "inf", ""}) {
            CHECK_MSG(mech.handle({{"rate_ml_h", bad}}, at(11)).rejected == 1,
                      std::string("taxa aceita: '") + bad + "'");
        }
        CHECK_NEAR(model.commandedRate(), 100.0, 0.0);
        // A refused message has no effect at all: the valid rate in it is not
        // applied.
        CHECK_EQ(static_cast<int>(mech.handle({{"rate_ml_h", "50"}, {"zzz", "1"}}, at(12)).rejected), 2);
        CHECK_NEAR(model.commandedRate(), 100.0, 0.0);
        // Rest wins over a rate in the same message.
        CHECK_EQ(static_cast<int>(mech.handle({{"rate_ml_h", "200"}, {"safe_state", "1"}}, at(13)).rejected),
                 0);
        CHECK_NEAR(model.commandedRate(), 0.0, 0.0);
        mech.handle({{"rate_ml_h", "100"}}, at(14));
    }

    medtest::section("Mecanismo - homem-morto (INF-09)");
    {
        // Heartbeats every 200 ms for five seconds, checked every 5 ms: never
        // trips.
        long long now = 14;
        for (int beat = 0; beat < 25; ++beat) {
            for (int k = 0; k < 40; ++k) {
                now += 5;
                mech.tick(at(now));
            }
            mech.handle({{"heartbeat", std::to_string(beat + 1)}}, at(now));
        }
        CHECK(!mech.tripped());
        CHECK_NEAR(model.commandedRate(), 100.0, 0.0);

        // Silence. Exactly the window does not trip; one millisecond past it
        // does, and the motor stops.
        const long long last = now;
        mech.tick(at(last + 600));
        CHECK(!mech.tripped());
        CHECK_NEAR(model.commandedRate(), 100.0, 0.0);
        mech.tick(at(last + 601));
        CHECK(mech.tripped());
        CHECK_NEAR(model.commandedRate(), 0.0, 0.0);

        // A trip is not undone by the controller coming back (INF-11).
        CHECK(mech.handle({{"heartbeat", "99"}}, at(last + 700)).detail == "dead-man tripped; reconnect");
        CHECK(mech.handle({{"rate_ml_h", "100"}}, at(last + 701)).rejected != 0);
        CHECK_NEAR(model.commandedRate(), 0.0, 0.0);

        // Only a new session, armed again, may command it.
        mech.connected(at(last + 800));
        CHECK(!mech.armed() && !mech.tripped());
        CHECK(mech.handle({{"rate_ml_h", "100"}}, at(last + 801)).rejected != 0);
        CHECK_EQ(static_cast<int>(mech.handle({{"deadman_ms", "600"}}, at(last + 802)).rejected), 0);
        CHECK_EQ(static_cast<int>(mech.handle({{"rate_ml_h", "100"}}, at(last + 803)).rejected), 0);

        // A refused message is still a proof of life.
        mech.tick(at(last + 803 + 500));
        mech.handle({{"unknown", "1"}}, at(last + 803 + 500));
        mech.tick(at(last + 803 + 1000));
        CHECK(!mech.tripped());

        // Disconnection rests at once, with no window to wait for.
        mech.disconnected();
        CHECK_NEAR(model.commandedRate(), 0.0, 0.0);
        CHECK(!mech.armed());
    }
}

// ------------------------------------------------------------ the process

struct Running {
    explicit Running(const medtest::TempDir& dir) {
        config.commandPath = dir.file("command.sock");
        config.dataPath = dir.file("data.sock");
        config.faultPath = dir.file("fault.sock");
        config.frameRateHz = 20.0;
        thread = std::thread([this] { status = runSimulator(config, stop, &error); });
        std::this_thread::sleep_for(milliseconds(100));
    }
    ~Running() {
        stop = true;
        thread.join();
    }
    SimulatorConfig config;
    std::atomic<bool> stop{false};
    std::thread thread;
    int status = -1;
    std::string error;
};

struct Readback {
    double encoderMm = 0, pressureMmHg = 0;
    bool air = false, motor = false, tripped = false;
};

Readback decode(const med::SampleFrame& frame) {
    Readback r;
    r.encoderMm = frame.at(0, 0) / 1e6;
    r.pressureMmHg = frame.at(1, 0) / 1e6;
    r.air = frame.at(2, 0) > 0.5e6;
    r.motor = frame.at(3, 0) > 0.5e6;
    r.tripped = frame.at(4, 0) > 0.5e6;
    return r;
}

/// Read frames, heartbeating every 200 ms if `beat`, until `done` says so or
/// `limit` passes. Returns the last readback and whether `done` was met.
template <typename Done>
bool readUntil(med::MedicalDevice& device, med::MedicalActuator* beat, milliseconds limit,
               Done done, Readback& last) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    auto nextBeat = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        if (beat && std::chrono::steady_clock::now() >= nextBeat) {
            beat->heartbeat();
            nextBeat += milliseconds(200);
        }
        med::Result<med::SampleFrame> frame = device.read(milliseconds(100));
        if (frame) {
            last = decode(frame.value());
            if (done(last)) {
                return true;
            }
        }
    }
    return false;
}

med::Status inject(const std::string& path, const std::string& key, const std::string& value) {
    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = path;
    auto channel = med::MedicalIpcChannel::connect(endpoint);
    if (!channel) {
        return channel.status();
    }
    auto message = med::amp::encodeControl({{key, value}});
    channel.value()->send(message.value().data(), message.value().size());
    unsigned char buffer[256];
    auto received = channel.value()->receive(buffer, sizeof(buffer), milliseconds(1000));
    if (!received) {
        return received.status();
    }
    auto ack = med::amp::decodeControlAck(buffer, received.value());
    return ack && ack.value().rejectedIndex == 0 ? med::Status::Ok : med::Status::InvalidArgument;
}

void testProcess() {
    medtest::section("Processo - o simulador atrás dos drivers do framework");

    medtest::TempDir dir;
    CHECK(dir.valid());
    Running sim(dir);

    med::DeviceConfig readbackConfig;
    readbackConfig.driver = "seqpacket";
    readbackConfig.id = "mechanism0";
    readbackConfig.address = sim.config.dataPath;
    readbackConfig.channelCount = kReadbackChannels;
    readbackConfig.sampleRateHz = sim.config.frameRateHz;
    readbackConfig.samplesPerFrame = 1;
    readbackConfig.channelUnits.assign(kReadbackUnits, kReadbackUnits + kReadbackChannels);
    auto device = med::MedicalDeviceFactory::create(readbackConfig);
    CHECK_MSG(device.isOk(), device.message());

    med::ActuatorConfig actuatorConfig;
    actuatorConfig.address = sim.config.commandPath;
    auto actuator = med::MedicalActuatorFactory::create(actuatorConfig);
    CHECK_MSG(actuator.isOk(), actuator.message());
    if (!device || !actuator) {
        return;
    }
    med::MedicalDevice& readback = *device.value();
    med::MedicalActuator& motor = *actuator.value();

    CHECK(readback.info().unitOf(1) == "mmHg");
    CHECK_STATUS(readback.start(), med::Status::Ok);
    CHECK_STATUS(motor.start(), med::Status::Ok);

    // A setpoint the mechanism cannot honour is refused through the
    // framework, with the mechanism's reason.
    CHECK_STATUS(motor.command({{"rate_ml_h", "5000"}}), med::Status::InvalidArgument);
    CHECK(motor.lastError().find("range") != std::string::npos);

    CHECK_STATUS(motor.command({{"rate_ml_h", "100"}}), med::Status::Ok);
    Readback r;
    CHECK(readUntil(readback, &motor, milliseconds(3000),
                    [](const Readback& x) { return x.motor && x.encoderMm > 0.0; }, r));
    const double moved = r.encoderMm;
    CHECK(readUntil(readback, &motor, milliseconds(3000),
                    [&](const Readback& x) { return x.encoderMm > moved; }, r));
    CHECK(r.pressureMmHg > 0.0 && !r.tripped);

    medtest::section("Processo - homem-morto pelo soquete (INF-09)");
    {
        // The controller stops heartbeating and stays connected: the hang the
        // timer exists for. Still running at 400 ms; stopped and tripped by
        // the window (600 ms) plus a frame period and the loop's tick.
        // Timed from a heartbeat sent here, the last one: the window runs
        // from the last proof of life, not from when the test stopped sending.
        CHECK_STATUS(motor.heartbeat(), med::Status::Ok);
        const auto silentSince = std::chrono::steady_clock::now();
        Readback seen;
        CHECK(!readUntil(readback, nullptr, milliseconds(400),
                         [](const Readback& x) { return !x.motor; }, seen));
        CHECK(seen.motor);
        CHECK(readUntil(readback, nullptr, milliseconds(2000),
                        [](const Readback& x) { return x.tripped; }, seen));
        const auto took = std::chrono::duration_cast<milliseconds>(
            std::chrono::steady_clock::now() - silentSince);
        CHECK(!seen.motor);
        CHECK(took >= milliseconds(600));
        CHECK(took <= milliseconds(900));
        std::printf("   homem-morto: motor parado %lld ms depois do último sinal (janela 600)\n",
                    static_cast<long long>(took.count()));

        // The controller comes back: refused, and the motor stays at rest.
        CHECK_STATUS(motor.heartbeat(), med::Status::InvalidArgument);
        CHECK(motor.lastError().find("tripped") != std::string::npos);
        CHECK(readUntil(readback, nullptr, milliseconds(500),
                        [](const Readback& x) { return !x.motor; }, seen));

        // Only a new session may command it again.
        motor.stop();
        CHECK_STATUS(motor.start(), med::Status::Ok);
        CHECK(readUntil(readback, &motor, milliseconds(1000),
                        [](const Readback& x) { return !x.tripped && !x.motor; }, seen));
    }

    medtest::section("Processo - falhas injetadas pelo canal de falhas");
    {
        // Occlusion at 1000 mL/h: resting 200 mmHg, 500 in about a second.
        CHECK_STATUS(motor.command({{"rate_ml_h", "1000"}}), med::Status::Ok);
        CHECK(readUntil(readback, &motor, milliseconds(3000),
                        [](const Readback& x) { return x.pressureMmHg > 150.0; }, r));
        CHECK_STATUS(inject(sim.config.faultPath, "occlusion", "1"), med::Status::Ok);
        CHECK(readUntil(readback, &motor, milliseconds(4000),
                        [](const Readback& x) { return x.pressureMmHg >= 500.0; }, r));
        CHECK_STATUS(inject(sim.config.faultPath, "occlusion", "0"), med::Status::Ok);

        // Air.
        CHECK_STATUS(inject(sim.config.faultPath, "air", "1"), med::Status::Ok);
        CHECK(readUntil(readback, &motor, milliseconds(1000),
                        [](const Readback& x) { return x.air; }, r));
        CHECK_STATUS(inject(sim.config.faultPath, "air", "0"), med::Status::Ok);

        // Stall: driven, and the encoder still.
        CHECK_STATUS(inject(sim.config.faultPath, "stall", "1"), med::Status::Ok);
        readUntil(readback, &motor, milliseconds(300), [](const Readback&) { return false; }, r);
        const double jammedAt = r.encoderMm;
        readUntil(readback, &motor, milliseconds(1000), [](const Readback&) { return false; }, r);
        CHECK(r.motor);
        CHECK_NEAR(r.encoderMm, jammedAt, 1e-9);
        CHECK_STATUS(inject(sim.config.faultPath, "stall", "0"), med::Status::Ok);

        // An unknown fault is refused.
        CHECK_STATUS(inject(sim.config.faultPath, "fire", "1"), med::Status::InvalidArgument);

        // The producer hangs: the controller's next exchange times out within
        // one heartbeat period (INF-10 begins here), and the frames stop.
        CHECK_STATUS(inject(sim.config.faultPath, "silent", "1"), med::Status::Ok);
        CHECK_STATUS(motor.heartbeat(), med::Status::Timeout);
        CHECK(!motor.isRunning());
        std::this_thread::sleep_for(milliseconds(200));
        while (readback.read(milliseconds(0))) {
        }
        CHECK_STATUS(readback.read(milliseconds(300)).status(), med::Status::Timeout);
        CHECK_STATUS(inject(sim.config.faultPath, "silent", "0"), med::Status::Ok);
    }

    motor.stop();
    readback.stop();
}

}  // namespace

int main() {
    std::printf("infusion-sim - verificações de host\n");
    testModel();
    testMechanism();
    testProcess();
    const int status = medtest::report();
    std::printf("\nO que estas verificações NÃO dizem: nada sobre uma bomba real. Cada número é do\n"
                "modelo (docs/REQUISITOS_INFUSAO.md §2).\n");
    return status;
}
