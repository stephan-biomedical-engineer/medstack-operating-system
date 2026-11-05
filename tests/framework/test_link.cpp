// SPDX-License-Identifier: MIT
//
// The producer link: the code the "rpmsg" and "seqpacket" drivers share,
// against a producer on a real socket (fake_producer.h).
//
// Until the "seqpacket" driver existed, none of this ran anywhere but on a
// board: the host has no rpmsg, so the connection window, the wait for the
// control acknowledgement, the backlog discarded ahead of it, and the rate the
// producer has to keep were all exercised only by bench sessions. They are one
// implementation with the transport as a parameter, so what is tested here is
// the rpmsg link's logic too - and the transport underneath is the one thing
// that is not.

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "MedicalDevice.h"
#include "check.h"
#include "fake_producer.h"
#include "temp_dir.h"

using namespace med;

namespace {

DeviceConfig linkConfig(const std::string& path) {
    DeviceConfig config;
    config.driver = "seqpacket";
    config.id = "mechanism0";
    config.address = path;
    config.channelCount = 8;
    config.sampleRateHz = 250.0;
    config.samplesPerFrame = 14;
    return config;
}

std::unique_ptr<MedicalDevice> make(const DeviceConfig& config) {
    Result<std::unique_ptr<MedicalDevice>> created = MedicalDeviceFactory::create(config);
    return created ? created.take() : nullptr;
}

}  // namespace

void testLink() {
    medtest::section("Enlace com produtor (seqpacket) - configurar e receber");

    medtest::TempDir dir;
    CHECK(dir.valid());

    {
        // Options are sent, the answer is awaited, and frames follow.
        medtest::FakeProducer::Script script;
        script.framesAfterAck = 5;
        script.sampleValue = 12.5;
        medtest::FakeProducer producer(dir.file("ok.sock"), script);
        CHECK(producer.listening());

        DeviceConfig config = linkConfig(dir.file("ok.sock"));
        config.driverOptions = {{"rate_setting", "fast"}, {"mode", "normal"}};
        std::unique_ptr<MedicalDevice> device = make(config);
        CHECK(device != nullptr);
        if (device) {
            CHECK(device->info().driver == "seqpacket");
            CHECK(device->info().model == "MedPlatform producer link (seqpacket)");
            CHECK_STATUS(device->start(), Status::Ok);
            CHECK(device->isRunning());
            for (std::uint64_t expected = 1; expected <= 5; ++expected) {
                Result<SampleFrame> frame = device->read(std::chrono::milliseconds(1000));
                CHECK_MSG(frame.isOk(), frame.message());
                if (!frame) {
                    break;
                }
                CHECK_EQ(static_cast<long long>(frame.value().sequence),
                         static_cast<long long>(expected));
                CHECK_NEAR(frame.value().at(7, 13), 12.5, 1e-9);
            }
            CHECK(device->lastError().empty());
            device->stop();
            CHECK(!device->isRunning());
        }
        producer.stop();
        const std::vector<std::map<std::string, std::string>> seen = producer.controls();
        CHECK_EQ(static_cast<long long>(seen.size()), 1LL);
        CHECK(!seen.empty() && seen.front() == config.driverOptions);
    }

    {
        // Frames queued ahead of the answer belong to no session and are
        // discarded; the start still succeeds, and the first frame read is
        // one sent after the acknowledgement.
        medtest::FakeProducer::Script script;
        script.framesBeforeAck = 3;
        script.framesAfterAck = 1;
        medtest::FakeProducer producer(dir.file("backlog.sock"), script);
        DeviceConfig config = linkConfig(dir.file("backlog.sock"));
        config.driverOptions = {{"mode", "normal"}};
        std::unique_ptr<MedicalDevice> device = make(config);
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::Ok);
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(1000));
            CHECK(frame.isOk());
            CHECK_EQ(static_cast<long long>(frame.value().sequence), 4LL);
        }
    }

    {
        // No options, nothing to answer: a producer that only streams.
        medtest::FakeProducer::Script script;
        script.framesAfterAck = 2;
        medtest::FakeProducer producer(dir.file("plain.sock"), script);
        std::unique_ptr<MedicalDevice> device = make(linkConfig(dir.file("plain.sock")));
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::Ok);
            CHECK(device->read(std::chrono::milliseconds(1000)).isOk());
        }
        producer.stop();
        CHECK(producer.controls().empty());
    }

    medtest::section("Enlace com produtor (seqpacket) - recusas");

    {
        // A refused option fails the start, names the option and the
        // producer's reason, and leaves the device stopped.
        medtest::FakeProducer::Script script;
        script.rejectIndex = 2;
        script.detail = "no such setting";
        medtest::FakeProducer producer(dir.file("refuse.sock"), script);
        DeviceConfig config = linkConfig(dir.file("refuse.sock"));
        config.driverOptions = {{"a_setting", "1"}, {"b_setting", "2"}};
        std::unique_ptr<MedicalDevice> device = make(config);
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::InvalidArgument);
            CHECK(!device->isRunning());
            CHECK_MSG(device->lastError().find("option 1") != std::string::npos &&
                          device->lastError().find("no such setting") != std::string::npos,
                      device->lastError());
        }
    }

    {
        // A producer that never answers is a timeout, within the wait the
        // driver declares (one second), not a hang.
        medtest::FakeProducer::Script script;
        script.silent = true;
        medtest::FakeProducer producer(dir.file("silent.sock"), script);
        DeviceConfig config = linkConfig(dir.file("silent.sock"));
        config.driverOptions = {{"mode", "normal"}};
        std::unique_ptr<MedicalDevice> device = make(config);
        CHECK(device != nullptr);
        if (device) {
            const auto before = std::chrono::steady_clock::now();
            CHECK_STATUS(device->start(), Status::Timeout);
            const auto took = std::chrono::steady_clock::now() - before;
            CHECK(took < std::chrono::milliseconds(2000));
            CHECK(!device->isRunning());
        }
    }

    {
        // A producer converting at another rate than the one configured is a
        // fault on the first frame: the record would be wrong about its own
        // time axis with every frame of it valid.
        medtest::FakeProducer::Script script;
        script.framesAfterAck = 1;
        script.rateHz = 500.0;
        medtest::FakeProducer producer(dir.file("rate.sock"), script);
        std::unique_ptr<MedicalDevice> device = make(linkConfig(dir.file("rate.sock")));
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::Ok);
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(1000));
            CHECK_STATUS(frame.status(), Status::OutOfRange);
            CHECK(device->lastError().find("500") != std::string::npos);
        }
    }

    {
        // Nobody listening: the driver retries for its window and then fails,
        // rather than failing at once (a producer that comes up after the
        // service is normal) or for ever (a crash loop that never reaches
        // "failed" is how this platform once hid a broken service).
        std::unique_ptr<MedicalDevice> device = make(linkConfig(dir.file("absent.sock")));
        CHECK(device != nullptr);
        if (device) {
            const auto before = std::chrono::steady_clock::now();
            const Status started = device->start();
            const auto took = std::chrono::steady_clock::now() - before;
            CHECK(started != Status::Ok);
            CHECK(took >= std::chrono::milliseconds(4500));
            CHECK(took < std::chrono::milliseconds(7000));
            CHECK(!device->isRunning());
        }
    }
}
