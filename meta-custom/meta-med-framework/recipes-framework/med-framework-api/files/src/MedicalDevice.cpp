// SPDX-License-Identifier: MIT

#include "MedicalDevice.h"

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include "MedicalIPC.h"

namespace med {

namespace amp {

std::uint32_t crc32(const void* data, std::size_t length) noexcept {
    // Plain CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). The table is
    // built once on first use; the firmware side computes the same value, and
    // a mismatch means the frame is discarded rather than turned into a
    // waveform.
    static std::uint32_t table[256];
    static bool initialised = false;
    if (!initialised) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? ((value >> 1) ^ 0xEDB88320U) : (value >> 1);
            }
            table[i] = value;
        }
        initialised = true;
    }

    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

}  // namespace amp

namespace {

/// Not M_PI: that macro is hidden when the compiler is put in strict ISO mode,
/// and this library must build the same way under -std=c++17 and -std=gnu++17.
constexpr double kPi = 3.14159265358979323846;

/// Synthetic EEG source.
///
/// Exists so the acquisition service, the IPC path, the storage path and the
/// HMI can all be exercised on a target that has no analogue front-end - the
/// QEMU profile of the thesis. The signal is deterministic (fixed seed), which
/// makes a recorded session reproducible and therefore usable as a regression
/// fixture.
class SimulatedDevice : public MedicalDevice {
public:
    explicit SimulatedDevice(const DeviceConfig& config)
        : config_(config),
          framePeriod_(std::chrono::nanoseconds(
              static_cast<std::int64_t>(1e9 * config.samplesPerFrame / config.sampleRateHz))),
          noise_(-3.0F, 3.0F) {
        random_.seed(0x4D454547U);  // fixed: reproducible sessions
    }

    DeviceInfo info() const override {
        DeviceInfo info;
        info.id = config_.id;
        info.model = "MedPlatform simulated EEG front-end";
        info.serialNumber = "SIM-0000";
        info.driver = "simulated";
        info.channelCount = config_.channelCount;
        info.sampleRateHz = config_.sampleRateHz;
        info.unit = "uV";
        return info;
    }

    Status start() override {
        if (running_) {
            return Status::Ok;
        }
        running_ = true;
        nextFrame_ = std::chrono::steady_clock::now() + framePeriod_;
        return Status::Ok;
    }

    Status stop() override {
        running_ = false;
        return Status::Ok;
    }

    bool isRunning() const override { return running_; }

    Result<SampleFrame> read(std::chrono::milliseconds timeout) override {
        if (!running_) {
            return Result<SampleFrame>::fail(Status::Unavailable, "device is stopped");
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < nextFrame_) {
            const auto remaining = nextFrame_ - now;
            if (remaining > timeout) {
                std::this_thread::sleep_for(timeout);
                return Result<SampleFrame>::fail(Status::Timeout, "frame not ready");
            }
            std::this_thread::sleep_for(remaining);
        }

        SampleFrame frame;
        frame.sequence = ++sequence_;
        frame.timestamp = Clock::now();
        frame.channelCount = config_.channelCount;
        frame.samplesPerChannel = config_.samplesPerFrame;
        frame.samples.resize(static_cast<std::size_t>(config_.samplesPerFrame) *
                             config_.channelCount);

        for (std::uint32_t i = 0; i < config_.samplesPerFrame; ++i) {
            const double t = static_cast<double>(sampleIndex_ + i) / config_.sampleRateHz;
            for (std::uint16_t c = 0; c < config_.channelCount; ++c) {
                const double phase = 0.4 * c;
                const double alpha = 20.0 * std::sin(2.0 * kPi * 10.0 * t + phase);
                const double mains = 5.0 * std::sin(2.0 * kPi * 50.0 * t);
                const double drift = 2.0 * std::sin(2.0 * kPi * 0.3 * t + phase);
                frame.samples[static_cast<std::size_t>(i) * config_.channelCount + c] =
                    static_cast<float>(alpha + mains + drift + noise_(random_));
            }
        }
        sampleIndex_ += config_.samplesPerFrame;

        nextFrame_ += framePeriod_;
        // If the consumer stalled, do not try to catch up by bursting frames
        // with stale timestamps - resynchronise instead. A gap in the record
        // is honest; fabricated timing is not.
        const auto after = std::chrono::steady_clock::now();
        if (nextFrame_ + 4 * framePeriod_ < after) {
            nextFrame_ = after + framePeriod_;
        }

        return Result<SampleFrame>::ok(std::move(frame));
    }

    Status selfTest() override {
        // Nothing physical to exercise; verify the configuration is one the
        // driver can actually honour, which is what the caller's self test
        // gate is really asking.
        if (config_.channelCount == 0 || config_.sampleRateHz <= 0.0 ||
            config_.samplesPerFrame == 0) {
            return Status::InvalidArgument;
        }
        return Status::Ok;
    }

private:
    DeviceConfig config_;
    std::chrono::steady_clock::duration framePeriod_;
    std::chrono::steady_clock::time_point nextFrame_{};
    std::uint64_t sequence_ = 0;
    std::uint64_t sampleIndex_ = 0;
    bool running_ = false;
    std::mt19937 random_;
    std::uniform_real_distribution<float> noise_;
};

/// AMP front-end: sample frames produced by firmware on the real-time core and
/// delivered over an OpenAMP rpmsg endpoint.
///
/// Note the size ceiling: a stock rpmsg buffer carries 512 bytes, ~496 of
/// payload. With a 40 byte header that is 114 int32 samples, so 8 channels x
/// 12 samples per frame fits and 8 x 25 does not. Frame geometry is a
/// negotiated property of the firmware interface, not a free parameter.
class RpmsgDevice : public MedicalDevice {
public:
    explicit RpmsgDevice(const DeviceConfig& config) : config_(config) {}

    DeviceInfo info() const override {
        DeviceInfo info;
        info.id = config_.id;
        info.model = "MedPlatform AMP front-end (rpmsg)";
        info.serialNumber = config_.address;
        info.driver = "rpmsg";
        info.channelCount = config_.channelCount;
        info.sampleRateHz = config_.sampleRateHz;
        info.unit = "uV";
        return info;
    }

    Status start() override {
        if (channel_) {
            return Status::Ok;
        }

        IpcEndpoint endpoint;
        endpoint.transport = IpcTransport::RpmsgChar;
        endpoint.address = config_.address.empty() ? "/dev/rpmsg0" : config_.address;

        Result<std::unique_ptr<MedicalIpcChannel>> channel =
            MedicalIpcChannel::connect(endpoint);
        if (!channel) {
            lastError_ = channel.message();
            return channel.status();
        }
        channel_ = channel.take();
        return Status::Ok;
    }

    Status stop() override {
        channel_.reset();
        return Status::Ok;
    }

    bool isRunning() const override { return static_cast<bool>(channel_); }

    Result<SampleFrame> read(std::chrono::milliseconds timeout) override {
        if (!channel_) {
            return Result<SampleFrame>::fail(Status::Unavailable, "device is stopped");
        }

        unsigned char buffer[2048];
        Result<std::size_t> received = channel_->receive(buffer, sizeof(buffer), timeout);
        if (!received) {
            return Result<SampleFrame>::fail(received.status(), received.message());
        }

        const std::size_t length = received.value();
        if (length < sizeof(amp::FrameHeader)) {
            return Result<SampleFrame>::fail(Status::IntegrityError, "short AMP frame");
        }

        amp::FrameHeader header;
        std::memcpy(&header, buffer, sizeof(header));

        if (header.magic != amp::kFrameMagic) {
            return Result<SampleFrame>::fail(Status::IntegrityError, "bad AMP frame magic");
        }
        if (header.version != amp::kFrameVersion) {
            return Result<SampleFrame>::fail(
                Status::NotSupported,
                "AMP frame version " + std::to_string(header.version) +
                    " is not supported by this framework build");
        }

        const std::size_t expected = static_cast<std::size_t>(header.channelCount) *
                                     header.samplesPerChannel * sizeof(std::int32_t);
        if (expected == 0 || length < sizeof(amp::FrameHeader) + expected) {
            return Result<SampleFrame>::fail(Status::IntegrityError,
                                             "AMP frame payload is truncated");
        }

        const unsigned char* payload = buffer + sizeof(amp::FrameHeader);
        if (amp::crc32(payload, expected) != header.crc32) {
            return Result<SampleFrame>::fail(Status::IntegrityError,
                                             "AMP frame failed its CRC check");
        }

        SampleFrame frame;
        frame.sequence = header.sequence;
        frame.timestamp = fromUnixMicros(header.timestampMicros);
        frame.channelCount = header.channelCount;
        frame.samplesPerChannel = header.samplesPerChannel;
        frame.samples.resize(static_cast<std::size_t>(header.channelCount) *
                             header.samplesPerChannel);

        // nano-units per LSB -> micro-units (the unit reported by info()).
        const double scale = static_cast<double>(header.scaleNanoUnitsPerLsb) / 1000.0;
        for (std::size_t i = 0; i < frame.samples.size(); ++i) {
            std::int32_t raw = 0;
            std::memcpy(&raw, payload + i * sizeof(std::int32_t), sizeof(raw));
            frame.samples[i] = static_cast<float>(raw * scale);
        }

        if (header.sequence != 0 && lastSequence_ != 0 &&
            header.sequence != lastSequence_ + 1) {
            // Report the gap; the caller decides whether a discontinuity in a
            // recording is acceptable for its clinical use.
            lastError_ = "sequence gap: expected " + std::to_string(lastSequence_ + 1) +
                         ", got " + std::to_string(header.sequence);
        }
        lastSequence_ = header.sequence;

        return Result<SampleFrame>::ok(std::move(frame));
    }

    Status selfTest() override {
        // The link itself is the thing under test: if the endpoint cannot be
        // opened, the co-processor is not running its firmware.
        const bool wasRunning = isRunning();
        const Status status = start();
        if (!wasRunning) {
            stop();
        }
        return status;
    }

private:
    DeviceConfig config_;
    std::unique_ptr<MedicalIpcChannel> channel_;
    std::uint64_t lastSequence_ = 0;
    std::string lastError_;
};

struct Registry {
    std::mutex mutex;
    std::map<std::string, MedicalDeviceFactory::Constructor> constructors;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

const bool kBuiltinDriversRegistered = [] {
    MedicalDeviceFactory::registerDriver(
        "simulated", [](const DeviceConfig& config) {
            return Result<std::unique_ptr<MedicalDevice>>::ok(
                std::unique_ptr<MedicalDevice>(new SimulatedDevice(config)));
        });
    MedicalDeviceFactory::registerDriver(
        "rpmsg", [](const DeviceConfig& config) {
            return Result<std::unique_ptr<MedicalDevice>>::ok(
                std::unique_ptr<MedicalDevice>(new RpmsgDevice(config)));
        });
    return true;
}();

}  // namespace

bool MedicalDeviceFactory::registerDriver(const std::string& name, Constructor constructor) {
    if (name.empty() || !constructor) {
        return false;
    }

    Registry& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mutex);
    if (reg.constructors.find(name) != reg.constructors.end()) {
        // Silently replacing a driver would mean the name in a configuration
        // file no longer identifies the code that will run.
        return false;
    }
    reg.constructors[name] = std::move(constructor);
    return true;
}

Result<std::unique_ptr<MedicalDevice>> MedicalDeviceFactory::create(const DeviceConfig& config) {
    using DeviceResult = Result<std::unique_ptr<MedicalDevice>>;

    (void)kBuiltinDriversRegistered;

    if (config.channelCount == 0) {
        return DeviceResult::fail(Status::InvalidArgument, "channelCount must be non-zero");
    }
    if (config.sampleRateHz <= 0.0) {
        return DeviceResult::fail(Status::InvalidArgument, "sampleRateHz must be positive");
    }
    if (config.samplesPerFrame == 0) {
        return DeviceResult::fail(Status::InvalidArgument, "samplesPerFrame must be non-zero");
    }

    Constructor constructor;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> guard(reg.mutex);
        const std::map<std::string, Constructor>::const_iterator it =
            reg.constructors.find(config.driver);
        if (it == reg.constructors.end()) {
            return DeviceResult::fail(Status::NotFound,
                                      "no MedicalDevice driver named '" + config.driver + "'");
        }
        constructor = it->second;
    }

    return constructor(config);
}

std::vector<std::string> MedicalDeviceFactory::drivers() {
    (void)kBuiltinDriversRegistered;

    Registry& reg = registry();
    std::lock_guard<std::mutex> guard(reg.mutex);

    std::vector<std::string> names;
    names.reserve(reg.constructors.size());
    for (const std::pair<const std::string, Constructor>& entry : reg.constructors) {
        names.push_back(entry.first);
    }
    return names;
}

}  // namespace med
