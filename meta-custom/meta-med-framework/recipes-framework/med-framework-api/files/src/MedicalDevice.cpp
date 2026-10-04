// SPDX-License-Identifier: MIT

#include "MedicalDevice.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

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

// ------------------------------------------------------------ driver options
//
// DeviceConfig::driverOptions is opaque to the framework as a whole. Each
// driver below owns the vocabulary of its own TRANSPORT, and nothing more:
//
//   simulated - the parameters of a synthetic converter (see SimulatedDevice)
//   rpmsg     - none: every option is forwarded to the producer, which
//               accepts or refuses it in its acknowledgement
//   iio       - the names of the device's own sysfs attributes
//
// What this layer does NOT have is a vocabulary for a class of device. Which
// settings a front-end has is the application's knowledge, and how a setting
// is spelled on a link is the producer's. A parser here that knew "gain",
// "reference" or "lead-off" would make every front-end that is not the one it
// was written for unconfigurable - on every driver at once.
//
// The rule that survives is the one that matters: a driver that cannot honour
// an option REFUSES it. Ignoring one would give a device running at a setting
// its configuration, its record and its operator all say it is not.

/// A whole-string number: no trailing text, no NaN, no infinity. An inline
/// comment that reached a value ("24   # PGA") is therefore not a number,
/// which is the behaviour a calibration value has to have.
bool parseNumber(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

std::vector<std::string> splitOn(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else if (c != ' ' && c != '\t') {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

// --------------------------------------------------------- control messages
//
// Serialising DeviceConfig::driverOptions into the fixed-layout message the
// producer decodes. Shared by every driver that has a producer on the other
// end of a link, which today is "rpmsg" and tomorrow is any firmware.

Result<std::vector<unsigned char>> encodeControl(
    const std::map<std::string, std::string>& options) {
    using EncodedResult = Result<std::vector<unsigned char>>;

    if (options.size() > amp::kControlMaxOptions) {
        return EncodedResult::fail(
            Status::InvalidArgument,
            "the control message carries at most " +
                std::to_string(amp::kControlMaxOptions) +
                " options and " + std::to_string(options.size()) +
                " were given. Truncating would send half a prescription");
    }

    amp::ControlMessage message;
    std::memset(&message, 0, sizeof(message));
    message.magic = amp::kControlMagic;
    message.version = amp::kControlVersion;
    message.optionCount = static_cast<std::uint16_t>(options.size());

    std::size_t index = 0;
    for (const std::pair<const std::string, std::string>& entry : options) {
        if (entry.first.size() >= amp::kControlKeySize ||
            entry.second.size() >= amp::kControlValueSize) {
            return EncodedResult::fail(Status::InvalidArgument,
                                       "control option '" + entry.first +
                                           "' does not fit the wire format");
        }
        std::memcpy(message.options[index].key, entry.first.data(), entry.first.size());
        std::memcpy(message.options[index].value, entry.second.data(),
                    entry.second.size());
        ++index;
    }

    message.crc32 = amp::crc32(message.options,
                               options.size() * sizeof(amp::ControlOption));

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&message);
    return EncodedResult::ok(std::vector<unsigned char>(bytes, bytes + sizeof(message)));
}

/// A synthetic converter.
///
/// Exists so the application, the IPC path, the storage path and the HMI can
/// all be exercised on a target that has no front-end - the QEMU profile of the
/// thesis. The signal is deterministic (fixed seed), which makes a recorded
/// session reproducible and therefore usable as a regression fixture.
///
/// It models a converter rather than an ideal source: it can quantise, clip at
/// full scale and add a noise floor, so defects that live in quantisation,
/// saturation or option handling show up on QEMU and not first on a bench. But
/// it models NO PARTICULAR converter and no particular signal. Every number
/// that makes it resemble one comes from driverOptions, which is to say from
/// the application's configuration:
///
///   unit             physical unit of the samples           default "a.u."
///   full_scale       clip at +-this, in `unit`              default 0 (none)
///   resolution_bits  quantise to full_scale / 2^(bits - 1)  default 0 (none)
///   noise_rms        gaussian noise, in `unit`              default 0
///   tones            "A@F[@P], ...": amplitude in `unit`, frequency in Hz and
///                    an optional phase step per channel in radians
///   square           "A@F": a +-A square wave on every channel instead of the
///                    tones - the shape of a built-in test generator
///
/// Any other key is refused, as on every driver.
class SimulatedDevice : public MedicalDevice {
public:
    static Result<std::unique_ptr<MedicalDevice>> create(const DeviceConfig& config) {
        using DeviceResult = Result<std::unique_ptr<MedicalDevice>>;

        Options options;
        for (const std::pair<const std::string, std::string>& entry : config.driverOptions) {
            const Status parsed = parseOption(entry.first, entry.second, options);
            if (parsed != Status::Ok) {
                return DeviceResult::fail(
                    parsed, parsed == Status::NotSupported
                                ? "the simulated driver has no option '" + entry.first +
                                      "'. Options are never ignored: a discarded setting "
                                      "is a device running at one nobody prescribed"
                                : "simulated option " + entry.first + " = '" +
                                      entry.second + "' is not valid");
            }
        }

        if (options.resolutionBits > 0 && !(options.fullScale > 0.0)) {
            return DeviceResult::fail(Status::InvalidArgument,
                                      "resolution_bits needs full_scale: a step is a "
                                      "fraction of a range, and there is no range");
        }

        return DeviceResult::ok(
            std::unique_ptr<MedicalDevice>(new SimulatedDevice(config, options)));
    }

    DeviceInfo info() const override {
        DeviceInfo info;
        info.id = config_.id;
        info.model = "MedPlatform simulated front-end";
        info.serialNumber = "SIM-0000";
        info.driver = "simulated";
        info.channelCount = config_.channelCount;
        info.sampleRateHz = config_.sampleRateHz;
        info.unit = options_.unit;
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
                double value = signalAt(t, c);
                if (options_.noiseRms > 0.0) {
                    value += options_.noiseRms * noise_(random_);
                }
                frame.samples[static_cast<std::size_t>(i) * config_.channelCount + c] =
                    static_cast<float>(quantise(value));
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
        if (config_.channelCount == 0 || config_.sampleRateHz <= 0.0 ||
            config_.samplesPerFrame == 0) {
            return Status::InvalidArgument;
        }

        // The generator and the converter model, checked against each other
        // over one second of every channel: every value finite, none beyond
        // full scale, every one on the quantisation grid. Nothing here depends
        // on what the configured signal represents - that is the application's
        // to know.
        const double step = stepSize();
        for (std::uint16_t c = 0; c < config_.channelCount; ++c) {
            for (int k = 0; k < 64; ++k) {
                const double value = quantise(signalAt(k / 64.0, c));
                if (!std::isfinite(value)) {
                    return Status::IntegrityError;
                }
                if (options_.fullScale > 0.0 && std::fabs(value) > options_.fullScale) {
                    return Status::IntegrityError;
                }
                if (step > 0.0) {
                    const double codes = value / step;
                    if (std::fabs(codes - std::floor(codes + 0.5)) > 1e-6) {
                        return Status::IntegrityError;
                    }
                }
            }
        }

        return Status::Ok;
    }

private:
    struct Tone {
        double amplitude = 0.0;
        double frequencyHz = 0.0;
        double phaseStepPerChannel = 0.0;
    };

    struct Options {
        std::string unit = "a.u.";
        double fullScale = 0.0;
        unsigned int resolutionBits = 0;
        double noiseRms = 0.0;
        std::vector<Tone> tones;
        bool square = false;
        Tone squareWave;
    };

    /// "A@F" or "A@F@P" - amplitude, non-negative frequency, optional phase.
    static bool parseTone(const std::string& text, bool allowPhase, Tone& out) {
        const std::vector<std::string> fields = splitOn(text, '@');
        if (fields.size() < 2 || fields.size() > (allowPhase ? 3U : 2U)) {
            return false;
        }
        Tone tone;
        if (!parseNumber(fields[0], tone.amplitude) ||
            !parseNumber(fields[1], tone.frequencyHz) || tone.frequencyHz < 0.0) {
            return false;
        }
        if (fields.size() == 3 && !parseNumber(fields[2], tone.phaseStepPerChannel)) {
            return false;
        }
        out = tone;
        return true;
    }

    static Status parseOption(const std::string& key, const std::string& value,
                              Options& options) {
        double number = 0.0;
        if (key == "unit") {
            if (value.empty()) {
                return Status::InvalidArgument;
            }
            options.unit = value;
        } else if (key == "full_scale") {
            if (!parseNumber(value, number)) {
                return Status::InvalidArgument;
            }
            if (number <= 0.0) {
                return Status::OutOfRange;
            }
            options.fullScale = number;
        } else if (key == "resolution_bits") {
            if (!parseNumber(value, number) || number != std::floor(number)) {
                return Status::InvalidArgument;
            }
            if (number < 1.0 || number > 32.0) {
                return Status::OutOfRange;
            }
            options.resolutionBits = static_cast<unsigned int>(number);
        } else if (key == "noise_rms") {
            if (!parseNumber(value, number)) {
                return Status::InvalidArgument;
            }
            if (number < 0.0) {
                return Status::OutOfRange;
            }
            options.noiseRms = number;
        } else if (key == "tones") {
            options.tones.clear();
            for (const std::string& entry : splitOn(value, ',')) {
                Tone tone;
                if (!parseTone(entry, true, tone)) {
                    return Status::InvalidArgument;
                }
                options.tones.push_back(tone);
            }
        } else if (key == "square") {
            if (!parseTone(value, false, options.squareWave)) {
                return Status::InvalidArgument;
            }
            options.square = true;
        } else {
            return Status::NotSupported;
        }
        return Status::Ok;
    }

    SimulatedDevice(const DeviceConfig& config, const Options& options)
        : config_(config),
          options_(options),
          framePeriod_(std::chrono::nanoseconds(
              static_cast<std::int64_t>(1e9 * config.samplesPerFrame / config.sampleRateHz))),
          noise_(0.0, 1.0) {
        random_.seed(0x4D454547U);  // fixed: reproducible sessions
    }

    /// The configured signal at time t on one channel, without noise.
    double signalAt(double t, std::uint16_t channel) const {
        if (options_.square) {
            // Every channel sees the generator: a built-in test generator is
            // routed to the inputs as a whole, not to one of them.
            const double phase = std::fmod(t * options_.squareWave.frequencyHz, 1.0);
            return (phase < 0.5) ? options_.squareWave.amplitude
                                 : -options_.squareWave.amplitude;
        }

        double value = 0.0;
        for (const Tone& tone : options_.tones) {
            value += tone.amplitude * std::sin(2.0 * kPi * tone.frequencyHz * t +
                                               tone.phaseStepPerChannel * channel);
        }
        return value;
    }

    /// One code of the modelled converter, or 0 when it does not quantise.
    double stepSize() const {
        if (options_.resolutionBits == 0) {
            return 0.0;
        }
        return options_.fullScale / std::ldexp(1.0, static_cast<int>(options_.resolutionBits) - 1);
    }

    /// Quantise to the converter's step and clip at its full scale.
    ///
    /// Saturating and not wrapping: a saturated sample is a visible artefact an
    /// operator can recognise, and a wrapped one is a waveform that looks like
    /// a signal and is not.
    double quantise(double value) const {
        if (options_.fullScale > 0.0) {
            value = std::max(-options_.fullScale, std::min(options_.fullScale, value));
        }
        const double step = stepSize();
        if (step > 0.0) {
            value = std::floor(value / step + 0.5) * step;
        }
        return value;
    }

    DeviceConfig config_;
    Options options_;
    std::chrono::steady_clock::duration framePeriod_;
    std::chrono::steady_clock::time_point nextFrame_{};
    std::uint64_t sequence_ = 0;
    std::uint64_t sampleIndex_ = 0;
    bool running_ = false;
    std::mt19937 random_;
    std::normal_distribution<double> noise_;
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
    /// No option is interpreted here. Their meaning belongs to the producer,
    /// which accepts or refuses each one in its acknowledgement - that is what
    /// the control protocol exists for, and a framework that judged the keys
    /// first would make the protocol dead code for every front-end it did not
    /// happen to know.
    ///
    /// What IS checked here is that the set fits the control message, so a
    /// configuration that could never be sent fails at creation, identically on
    /// every target, rather than at the first start() against firmware.
    static Result<std::unique_ptr<MedicalDevice>> create(const DeviceConfig& config) {
        using DeviceResult = Result<std::unique_ptr<MedicalDevice>>;

        if (!config.driverOptions.empty()) {
            Result<std::vector<unsigned char>> encoded = encodeControl(config.driverOptions);
            if (!encoded) {
                return DeviceResult::fail(encoded.status(), encoded.message());
            }
        }

        return DeviceResult::ok(std::unique_ptr<MedicalDevice>(new RpmsgDevice(config)));
    }

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

        // A bounded reconnection window, not a single attempt.
        //
        // On both hardware links the producer comes up after Linux does - the
        // co-processor is started by remoteproc, and a USB bridge enumerates
        // when it is plugged in - so an endpoint that is absent at the instant
        // the service starts is not the same thing as an endpoint that is not
        // coming. Retrying here, inside the driver, keeps the two units
        // independent: the alternative is an After= or a Requires= naming the
        // producer's unit, which would be the application layer knowing which
        // piece of hardware it is running on.
        //
        // Bounded, because the failure has to remain a failure. Restart=on-
        // failure plus a crash loop is how this platform has already hidden a
        // broken service from `systemctl list-units --state=failed`
        // (BRINGUP_STM32MP2.md, 163 restarts at 2.5 s), and the acceptance
        // suite's NRestarts assertion only means something if the number it
        // reads is zero for a healthy start.
        Status lastStatus = Status::Unavailable;
        const auto deadline = std::chrono::steady_clock::now() + kConnectWindow;
        for (;;) {
            Result<std::unique_ptr<MedicalIpcChannel>> channel =
                MedicalIpcChannel::connect(endpoint);
            if (channel) {
                channel_ = channel.take();
                break;
            }
            lastError_ = channel.message();
            lastStatus = channel.status();
            if (std::chrono::steady_clock::now() >= deadline) {
                return lastStatus;
            }
            std::this_thread::sleep_for(kConnectRetryInterval);
        }

        const Status configured = sendControl();
        if (configured != Status::Ok) {
            // A front-end that could not be programmed must not acquire. The
            // channel is dropped so the next start() is a clean attempt rather
            // than one against a half configured converter.
            channel_.reset();
            return configured;
        }

        return Status::Ok;
    }

    Status stop() override {
        channel_.reset();
        return Status::Ok;
    }

    bool isRunning() const override { return static_cast<bool>(channel_); }

    std::string lastError() const override { return lastError_; }

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
        // opened, the co-processor is not running its firmware. Whatever test
        // of the analogue path the front-end has runs on the far side and is
        // reported through the control acknowledgement, because only the
        // producer knows what its hardware should answer - this side has no
        // register map and must not acquire one.
        const bool wasRunning = isRunning();
        const Status status = start();
        if (!wasRunning) {
            stop();
        }
        return status;
    }

private:
    /// Hand the front-end settings to the producer and require an answer.
    ///
    /// Skipped entirely when there are no options: a firmware that speaks only
    /// frames stays usable, and the control protocol is something a producer
    /// opts into by being given something to configure. Once there IS an
    /// option, silence is a failure - the alternative is a converter running
    /// at whatever gain its reset value happens to give while the record says
    /// otherwise.
    Status sendControl() {
        if (config_.driverOptions.empty()) {
            return Status::Ok;
        }

        Result<std::vector<unsigned char>> encoded = encodeControl(config_.driverOptions);
        if (!encoded) {
            lastError_ = encoded.message();
            return encoded.status();
        }

        const Status sent = channel_->send(encoded.value().data(), encoded.value().size());
        if (sent != Status::Ok) {
            lastError_ = "the front-end settings could not be sent";
            return sent;
        }

        amp::ControlAck ack;
        std::memset(&ack, 0, sizeof(ack));
        Result<std::size_t> received =
            channel_->receive(&ack, sizeof(ack), kControlAckTimeout);
        if (!received) {
            lastError_ = "the front-end did not acknowledge its settings: " +
                         received.message();
            return received.status() == Status::Timeout ? Status::Timeout
                                                        : received.status();
        }

        if (received.value() < sizeof(ack) || ack.magic != amp::kControlAckMagic) {
            lastError_ = "the front-end answered the control message with something else";
            return Status::IntegrityError;
        }
        if (ack.version != amp::kControlVersion) {
            lastError_ = "the front-end speaks control protocol version " +
                         std::to_string(ack.version);
            return Status::NotSupported;
        }
        if (ack.rejectedIndex != 0) {
            ack.detail[amp::kControlDetailSize - 1] = '\0';
            lastError_ = "the front-end rejected option " +
                         std::to_string(ack.rejectedIndex - 1) + ": " +
                         std::string(ack.detail);
            return Status::InvalidArgument;
        }

        return Status::Ok;
    }

    /// Long enough for a co-processor to finish booting its firmware, short
    /// enough that a genuinely absent front-end is reported while an operator
    /// is still looking at the screen.
    static constexpr std::chrono::milliseconds kConnectWindow{5000};
    static constexpr std::chrono::milliseconds kConnectRetryInterval{250};
    static constexpr std::chrono::milliseconds kControlAckTimeout{1000};

    DeviceConfig config_;
    std::unique_ptr<MedicalIpcChannel> channel_;
    std::uint64_t lastSequence_ = 0;
    std::string lastError_;
};

/// A front-end the Linux kernel drives, reached through the industrial I/O ABI.
///
/// Two physical links can arrive here as the same device, and that is the
/// argument for this driver existing: a converter on a SoC bus with its
/// data-ready line wired to an interrupt, and one behind a USB bridge that
/// cannot deliver an interrupt at all, differ in the kernel and are identical
/// from this side.
///
/// Everything this driver knows is the IIO ABI - a character device, a buffer,
/// scan elements, a scale, a sampling frequency - and nothing about the part
/// behind it. The scale in particular is READ from the kernel and never
/// computed here: a conversion constant in userspace would be a datasheet
/// leaking two layers above the only one allowed to know it.
///
/// Options are the names of the device's own sysfs attributes, written and
/// then read back ("hardwaregain = 24"). Which attributes a device has is
/// decided by its kernel driver, and the configuration that names them is the
/// application's - so a front-end with a private attribute is configured
/// without this file learning its name.
///
/// No libiio. The framework has exactly two external dependencies and that is
/// a property of the architecture, not an accident of packaging: everything
/// below is open, read, poll and text files.
class IioDevice : public MedicalDevice {
public:
    static Result<std::unique_ptr<MedicalDevice>> create(const DeviceConfig& config) {
        using DeviceResult = Result<std::unique_ptr<MedicalDevice>>;

        for (const std::pair<const std::string, std::string>& entry : config.driverOptions) {
            // An attribute NAME, never a path: no '/', no '.', so an option
            // can reach this device's own directory and nothing else in sysfs.
            if (!isAttributeName(entry.first)) {
                return DeviceResult::fail(Status::InvalidArgument,
                                          "'" + entry.first +
                                              "' is not an industrial I/O attribute name");
            }
            // One setting, one source. The rate already comes from
            // DeviceConfig::sampleRateHz, and a second spelling of it could
            // disagree with the one the record declares.
            if (entry.first == "sampling_frequency") {
                return DeviceResult::fail(Status::InvalidArgument,
                                          "sampling_frequency is set from the sample rate, "
                                          "not as an option");
            }
            if (entry.second.empty() || entry.second.find('\n') != std::string::npos) {
                return DeviceResult::fail(Status::InvalidArgument,
                                          "option " + entry.first + " has no usable value");
            }
        }

        return DeviceResult::ok(std::unique_ptr<MedicalDevice>(new IioDevice(config)));
    }

    ~IioDevice() override { stop(); }

    DeviceInfo info() const override {
        DeviceInfo info;
        info.id = config_.id;
        info.model = chipName_.empty() ? "industrial I/O front-end"
                                       : "Linux IIO front-end (" + chipName_ + ")";
        info.serialNumber = devicePath_;
        info.driver = "iio";
        info.channelCount = config_.channelCount;
        info.sampleRateHz = config_.sampleRateHz;
        info.unit = "uV";
        return info;
    }

    Status start() override {
        if (fd_ >= 0) {
            return Status::Ok;
        }

        const Status resolved = resolve();
        if (resolved != Status::Ok) {
            return resolved;
        }

        const Status configured = applyOptions();
        if (configured != Status::Ok) {
            return configured;
        }

        const Status armed = enableBuffer();
        if (armed != Status::Ok) {
            return armed;
        }

        fd_ = ::open(devicePath_.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd_ < 0) {
            lastError_ = "open " + devicePath_ + ": " + std::strerror(errno);
            writeSysfs("buffer/enable", "0");
            return Status::IoError;
        }

        filled_ = 0;
        block_.assign(scanSize_ * config_.samplesPerFrame, 0);
        return Status::Ok;
    }

    Status stop() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (!sysfsPath_.empty()) {
            writeSysfs("buffer/enable", "0");
        }
        return Status::Ok;
    }

    bool isRunning() const override { return fd_ >= 0; }

    std::string lastError() const override { return lastError_; }

    Result<SampleFrame> read(std::chrono::milliseconds timeout) override {
        if (fd_ < 0) {
            return Result<SampleFrame>::fail(Status::Unavailable, "device is stopped");
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (filled_ < block_.size()) {
            const ssize_t n = ::read(fd_, block_.data() + filled_, block_.size() - filled_);
            if (n > 0) {
                // A short read is normal: the kernel hands over whole scans and
                // a frame is many of them. Accumulate rather than discard - the
                // alternative is dropping samples that were successfully
                // converted, which is data loss invented by the reader.
                filled_ += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                return Result<SampleFrame>::fail(Status::Unavailable,
                                                 "the front-end closed its buffer");
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return Result<SampleFrame>::fail(Status::IoError, std::strerror(errno));
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return Result<SampleFrame>::fail(Status::Timeout, "frame not ready");
            }

            struct pollfd waiting;
            waiting.fd = fd_;
            waiting.events = POLLIN;
            waiting.revents = 0;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const int ready = ::poll(&waiting, 1, static_cast<int>(remaining.count()));
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return Result<SampleFrame>::fail(Status::IoError, std::strerror(errno));
            }
            if (ready == 0) {
                return Result<SampleFrame>::fail(Status::Timeout, "frame not ready");
            }
        }

        SampleFrame frame = decode();
        filled_ = 0;
        return Result<SampleFrame>::ok(std::move(frame));
    }

    Status selfTest() override {
        const Status resolved = resolve();
        if (resolved != Status::Ok) {
            return resolved;
        }

        // Step 1 - identity. The kernel names the device, and a name here is
        // an assertion that a driver bound to something that answered.
        if (chipName_.empty()) {
            lastError_ = "the front-end reports no name";
            return Status::Unavailable;
        }

        // Step 2 - every configured channel exists and carries a scale. A
        // configuration asking for eight channels on a four channel part must
        // stop the service, not acquire four channels of signal and four of
        // whatever the buffer held.
        for (std::uint16_t channel = 0; channel < config_.channelCount; ++channel) {
            std::string ignored;
            if (!readSysfs("in_voltage" + std::to_string(channel) + "_scale", ignored)) {
                lastError_ = "the front-end has no channel " + std::to_string(channel);
                return Status::OutOfRange;
            }
        }

        // What is NOT here, deliberately: an exercise of the analogue path.
        // Routing a converter to its own test generator and judging what comes
        // back needs the generator's amplitude, the part's noise floor and the
        // names of its private controls - facts about one piece of silicon,
        // which this layer may not hold. The kernel driver holds them, and the
        // IIO convention is that it runs such a test at probe: a front-end that
        // fails it is never registered, and resolve() above reports it absent.
        return Status::Ok;
    }

private:
    explicit IioDevice(const DeviceConfig& config) : config_(config) {}

    static bool isAttributeName(const std::string& name) {
        if (name.empty()) {
            return false;
        }
        for (const char c : name) {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!allowed) {
                return false;
            }
        }
        return true;
    }

    /// Whether a value read back is the one written. Text first, then as
    /// numbers, because the kernel formats a number its own way: "24" may come
    /// back as "24.000000", and that is the same setting.
    static bool sameSetting(const std::string& written, const std::string& readBack) {
        if (written == readBack) {
            return true;
        }
        double a = 0.0;
        double b = 0.0;
        if (!parseNumber(written, a) || !parseNumber(readBack, b)) {
            return false;
        }
        return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(a));
    }

    // --------------------------------------------------------------- sysfs

    std::string attributePath(const std::string& relative) const {
        return sysfsPath_ + "/" + relative;
    }

    bool readSysfs(const std::string& relative, std::string& out) const {
        const int fd = ::open(attributePath(relative).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        char buffer[256];
        const ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
        ::close(fd);
        if (n < 0) {
            return false;
        }
        out.assign(buffer, static_cast<std::size_t>(n));
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
            out.pop_back();
        }
        return true;
    }

    bool writeSysfs(const std::string& relative, const std::string& value) const {
        const int fd = ::open(attributePath(relative).c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        const ssize_t n = ::write(fd, value.data(), value.size());
        ::close(fd);
        return n == static_cast<ssize_t>(value.size());
    }

    // ----------------------------------------------------------- resolution

    Status resolve() {
        const std::string requested =
            config_.address.empty() ? "/dev/iio:device0" : config_.address;

        // realpath, because the configured address is expected to be a udev
        // symlink naming the front-end rather than /dev/iio:deviceN, which is
        // numbered in probe order. A name that identifies a device is worth the
        // extra syscall; this repository has already paid for the general form
        // of that lesson with a partition label.
        char resolved[PATH_MAX];
        if (::realpath(requested.c_str(), resolved) == nullptr) {
            const int error = errno;
            lastError_ = "cannot resolve " + requested + ": " + std::strerror(error);
            if (error == ENOENT) {
                // Said out loud, because absence has two causes that look the
                // same from here: nothing is attached, or a kernel driver
                // refused to register a device that failed its own probe-time
                // checks. Only the kernel log tells them apart.
                lastError_ += ". No such device is registered; if one is attached, "
                              "its kernel driver refused it - see the kernel log";
                return Status::NotFound;
            }
            return Status::IoError;
        }
        devicePath_ = resolved;

        const std::string::size_type slash = devicePath_.find_last_of('/');
        const std::string node =
            slash == std::string::npos ? devicePath_ : devicePath_.substr(slash + 1);
        sysfsPath_ = "/sys/bus/iio/devices/" + node;

        struct stat info;
        if (::stat(sysfsPath_.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
            lastError_ = devicePath_ + " is not an industrial I/O device";
            return Status::NotSupported;
        }

        readSysfs("name", chipName_);
        return Status::Ok;
    }

    // ------------------------------------------------------------- settings

    Status applyOptions() {
        // Every option, written and read back, in key order. Written before
        // the scale is read, because an option may be the gain and reading the
        // scale first would record the previous setting.
        //
        // In KEY order, which is a constraint worth naming: a kernel driver
        // whose attributes only work when written in some other order cannot be
        // configured correctly from a map, and that belongs fixed in the
        // driver rather than papered over here.
        for (const std::pair<const std::string, std::string>& entry : config_.driverOptions) {
            if (::access(attributePath(entry.first).c_str(), F_OK) != 0) {
                lastError_ = "the front-end has no attribute '" + entry.first + "'";
                return Status::NotSupported;
            }
            if (!writeSysfs(entry.first, entry.second)) {
                lastError_ = "the front-end refused " + entry.first + " = '" + entry.second +
                             "'";
                return Status::OutOfRange;
            }
            // The read-back is for the other failure: a driver that accepts a
            // value and programs the nearest one. A device running at a setting
            // its configuration does not name is a traceability defect.
            std::string readBack;
            if (!readSysfs(entry.first, readBack)) {
                lastError_ = "cannot read back " + entry.first +
                             ", so the setting cannot be confirmed";
                return Status::NotSupported;
            }
            if (!sameSetting(entry.second, readBack)) {
                lastError_ = "asked for " + entry.first + " = '" + entry.second +
                             "' and the front-end programmed '" + readBack + "'";
                return Status::OutOfRange;
            }
        }

        {
            std::ostringstream requested;
            requested << static_cast<long long>(config_.sampleRateHz);
            if (!writeSysfs("sampling_frequency", requested.str())) {
                lastError_ = "the front-end refused a sample rate of " + requested.str() + " Hz";
                return Status::OutOfRange;
            }
            std::string readBack;
            if (readSysfs("sampling_frequency", readBack) &&
                !sameSetting(requested.str(), readBack)) {
                lastError_ = "asked for " + requested.str() + " samples per second and the "
                             "front-end programmed " + readBack;
                return Status::OutOfRange;
            }
        }

        // Scale, read from the kernel and never computed. in_voltage0_scale is
        // millivolts per LSB; the frames this framework hands out are in
        // microvolts.
        std::string scaleText;
        if (!readSysfs("in_voltage0_scale", scaleText)) {
            lastError_ = "the front-end does not publish a scale, so its samples "
                         "cannot be given a unit";
            return Status::NotSupported;
        }
        scaleMicrovoltsPerLsb_ = std::strtod(scaleText.c_str(), nullptr) * 1000.0;
        if (!(scaleMicrovoltsPerLsb_ > 0.0)) {
            lastError_ = "the front-end published a scale of '" + scaleText + "'";
            return Status::IntegrityError;
        }

        return Status::Ok;
    }

    // --------------------------------------------------------------- buffer

    Status enableBuffer() {
        writeSysfs("buffer/enable", "0");

        for (std::uint16_t channel = 0; channel < config_.channelCount; ++channel) {
            const std::string element =
                "scan_elements/in_voltage" + std::to_string(channel) + "_en";
            if (!writeSysfs(element, "1")) {
                lastError_ = "the front-end has no channel " + std::to_string(channel);
                return Status::OutOfRange;
            }
        }

        // The timestamp channel is requested and its absence is recorded rather
        // than fixed: a front-end whose samples carry no time of their own
        // still acquires, but the record must not later be read as if it did.
        hasTimestamp_ = writeSysfs("scan_elements/in_timestamp_en", "1");

        const Status layout = computeScanLayout();
        if (layout != Status::Ok) {
            return layout;
        }

        // Two frames of headroom in the kernel buffer, so that a frame being
        // decoded here does not cost a conversion there.
        if (!writeSysfs("buffer/length",
                        std::to_string(2 * config_.samplesPerFrame * config_.channelCount))) {
            lastError_ = "cannot size the front-end's buffer";
            return Status::IoError;
        }

        if (!writeSysfs("buffer/enable", "1")) {
            lastError_ = "the front-end refused to start its buffer";
            return Status::Unavailable;
        }

        return Status::Ok;
    }

    /// Work out how the kernel packs one scan, from what the kernel says.
    ///
    /// Deliberately parsed rather than assumed. The type string
    /// ("le:s24/32>>0") carries the storage size, the significant bits, the
    /// sign and the shift, and every one of them can differ between front-ends
    /// - guessing 32 bits here is how a driver silently reads a 16-bit
    /// converter as noise.
    Status computeScanLayout() {
        std::string type;
        if (!readSysfs("scan_elements/in_voltage0_type", type)) {
            lastError_ = "the front-end does not describe its sample format";
            return Status::NotSupported;
        }

        char sign = 's';
        unsigned int realBits = 0;
        unsigned int storageBits = 0;
        unsigned int shift = 0;
        const bool littleEndian = type.compare(0, 3, "le:") == 0;
        const char* cursor = type.c_str() + (type.find(':') + 1);
        if (std::sscanf(cursor, "%c%u/%u>>%u", &sign, &realBits, &storageBits, &shift) < 3) {
            lastError_ = "cannot parse the sample format '" + type + "'";
            return Status::NotSupported;
        }

        // The framework converts through a signed 32-bit path, and a front-end
        // that does not fit it is refused instead of being misread. Endianness
        // is checked and not swapped for the same reason: a converter that
        // disagrees with the host is a real configuration, and the honest
        // answer today is that this driver has never seen one.
        if (storageBits != 32 || sign != 's' || shift != 0 ||
            littleEndian != isHostLittleEndian()) {
            lastError_ = "unsupported sample format '" + type + "'";
            return Status::NotSupported;
        }
        realBits_ = realBits;

        const std::size_t voltageBytes =
            static_cast<std::size_t>(config_.channelCount) * sizeof(std::int32_t);
        if (hasTimestamp_) {
            // The kernel aligns each scan element to its own size, so the
            // 64-bit timestamp starts at the next multiple of 8.
            timestampOffset_ = (voltageBytes + 7U) & ~static_cast<std::size_t>(7U);
            scanSize_ = timestampOffset_ + sizeof(std::int64_t);
        } else {
            timestampOffset_ = 0;
            scanSize_ = voltageBytes;
        }

        return Status::Ok;
    }

    static bool isHostLittleEndian() {
        const std::uint16_t probe = 1;
        unsigned char first = 0;
        std::memcpy(&first, &probe, 1);
        return first == 1;
    }

    SampleFrame decode() {
        SampleFrame frame;
        frame.sequence = ++sequence_;
        frame.channelCount = config_.channelCount;
        frame.samplesPerChannel = config_.samplesPerFrame;
        frame.samples.resize(static_cast<std::size_t>(config_.samplesPerFrame) *
                             config_.channelCount);

        // The frame carries the time of its FIRST sample, because that is the
        // instant the block describes; deriving the others from the sample rate
        // is the consumer's business and is exact.
        if (hasTimestamp_) {
            std::int64_t nanos = 0;
            std::memcpy(&nanos, block_.data() + timestampOffset_, sizeof(nanos));
            frame.timestamp = fromUnixMicros(static_cast<std::uint64_t>(nanos / 1000));
        } else {
            frame.timestamp = Clock::now();
        }

        const int shift = 32 - static_cast<int>(realBits_);
        for (std::uint32_t sample = 0; sample < config_.samplesPerFrame; ++sample) {
            const unsigned char* scan = block_.data() + sample * scanSize_;
            for (std::uint16_t channel = 0; channel < config_.channelCount; ++channel) {
                std::int32_t raw = 0;
                std::memcpy(&raw, scan + channel * sizeof(std::int32_t), sizeof(raw));
                // Sign extend from the converter's real width. The kernel
                // right-aligns the value in 32 bits without extending it, so a
                // negative sample read as-is would be a large positive one -
                // a waveform that clips upwards on every trough.
                if (shift > 0) {
                    raw = static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(raw) << shift);
                    raw >>= shift;
                }
                frame.samples[static_cast<std::size_t>(sample) * config_.channelCount +
                              channel] =
                    static_cast<float>(static_cast<double>(raw) * scaleMicrovoltsPerLsb_);
            }
        }

        return frame;
    }

    DeviceConfig config_;
    std::string devicePath_;
    std::string sysfsPath_;
    std::string chipName_;
    int fd_ = -1;
    double scaleMicrovoltsPerLsb_ = 0.0;
    unsigned int realBits_ = 24;
    bool hasTimestamp_ = false;
    std::size_t scanSize_ = 0;
    std::size_t timestampOffset_ = 0;
    std::size_t filled_ = 0;
    std::uint64_t sequence_ = 0;
    std::vector<unsigned char> block_;
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
    MedicalDeviceFactory::registerDriver("simulated", &SimulatedDevice::create);
    MedicalDeviceFactory::registerDriver("rpmsg", &RpmsgDevice::create);
    MedicalDeviceFactory::registerDriver("iio", &IioDevice::create);
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
