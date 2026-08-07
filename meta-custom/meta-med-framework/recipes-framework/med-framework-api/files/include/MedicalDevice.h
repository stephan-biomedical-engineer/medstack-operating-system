// SPDX-License-Identifier: MIT
//
// MedFramework - biomedical sensor and actuator abstraction.
//
// The interface an application acquires signals through, and the reason an EEG
// service compiled once runs against a simulated source on QEMU and against a
// Cortex-M4 front-end on the STM32MP257. Drivers register themselves with
// MedicalDeviceFactory under a name; the name comes from configuration, so
// swapping the acquisition backend is a configuration change, not a code
// change.
//
// Drivers shipped with the framework:
//
//   "simulated" - synthetic EEG (alpha rhythm + mains interference + noise).
//                 The reference target for the QEMU profile and for testing
//                 the application without hardware.
//   "rpmsg"     - AMP front-end: sample frames produced by firmware on the
//                 real-time core, delivered over OpenAMP rpmsg via
//                 MedicalIpcChannel.
//
// A driver for a directly attached AFE/ADC (industrial I/O) plugs in the same
// way and is the natural extension point for a new device class.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MedicalTypes.h"

namespace med {

struct DeviceInfo {
    std::string id;
    std::string model;
    std::string serialNumber;
    std::string driver;
    std::uint16_t channelCount = 0;
    double sampleRateHz = 0.0;
    /// Physical unit of the values in SampleFrame::samples, e.g. "uV".
    std::string unit;
};

/// One block of samples from every channel, channel-interleaved:
/// samples[i * channelCount + c] is sample i of channel c.
struct SampleFrame {
    std::uint64_t sequence = 0;
    Timestamp timestamp{};
    std::uint16_t channelCount = 0;
    std::uint32_t samplesPerChannel = 0;
    std::vector<float> samples;

    /// Bounds checked accessor; returns 0 for an out of range request rather
    /// than reading past the buffer.
    float at(std::uint16_t channel, std::uint32_t index) const noexcept {
        const std::size_t offset =
            static_cast<std::size_t>(index) * channelCount + channel;
        if (channel >= channelCount || index >= samplesPerChannel ||
            offset >= samples.size()) {
            return 0.0F;
        }
        return samples[offset];
    }
};

struct DeviceConfig {
    std::string driver = "simulated";
    std::string id = "eeg0";
    /// Driver specific: "/dev/rpmsg0" for rpmsg, unused by simulated.
    std::string address;
    std::uint16_t channelCount = 8;
    double sampleRateHz = 250.0;
    /// Samples per channel in one frame. Trades latency against syscall rate;
    /// 25 at 250 Hz is a 100 ms frame.
    std::uint32_t samplesPerFrame = 25;
};

class MedicalDevice {
public:
    virtual ~MedicalDevice() = default;

    virtual DeviceInfo info() const = 0;

    /// Begin acquisition. Idempotent.
    virtual Status start() = 0;
    virtual Status stop() = 0;
    virtual bool isRunning() const = 0;

    /// Block for at most `timeout` for the next frame. Returns Timeout when
    /// none arrived - not an error, and the caller is expected to loop.
    virtual Result<SampleFrame> read(std::chrono::milliseconds timeout) = 0;

    /// Power-on / periodic self test (IEC 60601-1 §14 essential performance).
    /// Must be safe to call while stopped.
    virtual Status selfTest() = 0;
};

class MedicalDeviceFactory {
public:
    using Constructor =
        std::function<Result<std::unique_ptr<MedicalDevice>>(const DeviceConfig&)>;

    /// Returns true if the name was free. Registering an existing name fails
    /// rather than silently replacing a driver.
    static bool registerDriver(const std::string& name, Constructor constructor);

    static Result<std::unique_ptr<MedicalDevice>> create(const DeviceConfig& config);

    static std::vector<std::string> drivers();
};

/// Wire format of the AMP link. The firmware on the real-time core emits this
/// header followed by `channelCount * samplesPerChannel` little-endian int32
/// samples in LSB units; the "rpmsg" driver converts them with
/// scaleNanoUnitsPerLsb. Fixed layout: both sides are compiled by different
/// toolchains for different architectures.
namespace amp {

constexpr std::uint32_t kFrameMagic = 0x4D454547U;  // "MEEG"
constexpr std::uint16_t kFrameVersion = 1;

#pragma pack(push, 1)
struct FrameHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t channelCount;
    std::uint32_t samplesPerChannel;
    std::uint32_t sampleRateMilliHz;
    std::uint64_t sequence;
    std::uint64_t timestampMicros;
    std::int32_t scaleNanoUnitsPerLsb;
    std::uint32_t crc32;  ///< over the sample payload only
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 40, "AMP frame header layout is ABI");

std::uint32_t crc32(const void* data, std::size_t length) noexcept;

}  // namespace amp

}  // namespace med
