// SPDX-License-Identifier: MIT
//
// MedFramework - biomedical sensor and actuator abstraction.
//
// The interface an application acquires signals through, and the reason an
// acquisition service compiled once runs against a simulated source on QEMU and
// against a real-time-core front-end on the STM32MP257. Drivers register themselves with
// MedicalDeviceFactory under a name; the name comes from configuration, so
// swapping the acquisition backend is a configuration change, not a code
// change.
//
// Drivers shipped with the framework:
//
//   "simulated" - a synthetic converter whose signal, range, resolution and
//                 noise all come from the configuration. The reference target
//                 for the QEMU profile and for testing an application without
//                 hardware.
//   "rpmsg"     - AMP front-end: sample frames produced by firmware on the
//                 real-time core, delivered over OpenAMP rpmsg via
//                 MedicalIpcChannel.
//   "iio"       - an analogue front-end the Linux kernel drives itself,
//                 through the industrial I/O ABI: a character device for the
//                 sample buffer and sysfs for scale, rate and settings. Used both
//                 for a converter on a SoC SPI bus and for one behind a USB
//                 bridge - from here the two are the same device, which is the
//                 point of the kernel owning the link.
//
// Every driver name is the name of a TRANSPORT, never of a part. No driver here
// is named after a converter and none may be: the moment a part number reaches
// this layer, the claim that an application is portable across front-ends stops
// being checkable by grep.
//
// Note what "iio" costs and buys against "rpmsg", because the two are not
// interchangeable clinically. With rpmsg the converter belongs to the
// real-time core and Linux never touches it, which is the argument that lets
// the Linux software item carry a lower safety classification (IEC 62304
// §5.3). With iio the converter is inside the kernel. That is a deliberate,
// declared difference and the record says which one produced it - see
// acquisition.link in the application's configuration.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
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
    std::string id = "device0";
    /// Driver specific: "/dev/rpmsg0" for rpmsg, a /dev/iio:device* node (or a
    /// stable symlink to one) for iio, unused by simulated.
    std::string address;
    std::uint16_t channelCount = 8;
    double sampleRateHz = 250.0;
    /// Samples per channel in one frame. Trades latency against syscall rate;
    /// 25 at 250 Hz is a 100 ms frame.
    std::uint32_t samplesPerFrame = 25;

    /// Settings for the front-end, opaque to the framework as a whole.
    ///
    /// Each driver owns the vocabulary of its TRANSPORT and nothing more:
    ///   "simulated" - the synthetic converter's parameters (unit, full_scale,
    ///                 resolution_bits, noise_rms, tones, square)
    ///   "rpmsg"     - none; every option is forwarded to the producer, which
    ///                 accepts or refuses each in its ControlAck
    ///   "iio"       - names of the device's own sysfs attributes, each written
    ///                 and read back
    ///
    /// There is deliberately no vocabulary for a class of device. Which
    /// settings a front-end has is the application's knowledge and how one is
    /// spelled on a link is the producer's; a `gain` field here, or a parser
    /// that knew one, would make every front-end without a gain unconfigurable.
    ///
    /// What every driver must do is honour a key or refuse it. Silently
    /// dropping an option that happens to be a safety parameter is a defect,
    /// not a tolerance.
    std::map<std::string, std::string> driverOptions;
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

    /// What the last failed call refused, in words, for the audit record -
    /// "the front-end refused bias_drive = 'derived'" where the Status alone
    /// says OutOfRange. Empty when the driver has nothing to add.
    ///
    /// Last in the class on purpose: appended, it leaves every existing
    /// vtable slot where it was, so a caller built against the previous
    /// header still finds start() and selfTest() where it expects them.
    virtual std::string lastError() const { return {}; }
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

/// "MEEG" is a historical spelling and stays: the magic is ABI shared with
/// firmware, and a frame is a frame whatever the device measures.
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
    /// Nanovolts per LSB of the int32 samples that follow.
    ///
    /// Producers are expected to deliver NANOVOLTS and set this to 1, not to
    /// send raw converter counts with a rounded step. The reason is
    /// arithmetic: a high-resolution converter's step is rarely a whole number
    /// of nanovolts - 22.35 nV, say - and an integer field can only say 22, a
    /// 1.6% gain error on every sample of every trace, invisible because the
    /// waveform still looks plausible. One multiplication at the producer
    /// removes it and costs no ABI change: an int32 of nanovolts spans
    /// +-2.1 V.
    ///
    /// The field stays, and stays honest: a producer that genuinely has an
    /// integer step may still declare it.
    std::int32_t scaleNanoUnitsPerLsb;
    std::uint32_t crc32;  ///< over the sample payload only
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 40, "AMP frame header layout is ABI");

/// Control channel: the settings the application prescribes and the front-end
/// has to program into a converter's registers.
///
/// A second message type on the same link, with the same discipline as the
/// frame header - fixed layout, packed, size asserted - because it crosses the
/// same boundary between two toolchains and two architectures. It is
/// deliberately a list of opaque key/value strings and not a struct of named
/// settings: the framework must not acquire an opinion about what a front-end
/// has, and a struct would need a new field, and therefore a new ABI, for
/// every converter.
constexpr std::uint32_t kControlMagic = 0x4D435452U;   // "MCTR"
constexpr std::uint32_t kControlAckMagic = 0x4D435441U;  // "MCTA"
constexpr std::uint16_t kControlVersion = 1;

/// Sized against the transport, not against taste: a stock rpmsg buffer is 512
/// bytes with about 496 usable, and 8 options of 56 bytes plus a 16 byte
/// header is 464. Needing a ninth option is a protocol change - a producer
/// that reads a truncated option list would program a converter from half a
/// prescription, so the framework refuses to send rather than trimming.
constexpr std::uint16_t kControlMaxOptions = 8;
constexpr std::size_t kControlKeySize = 32;
constexpr std::size_t kControlValueSize = 24;
constexpr std::size_t kControlDetailSize = 64;

#pragma pack(push, 1)
struct ControlOption {
    char key[kControlKeySize];      ///< NUL terminated
    char value[kControlValueSize];  ///< NUL terminated
};

struct ControlMessage {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t optionCount;
    std::uint32_t crc32;  ///< over the first optionCount options only
    std::uint32_t reserved;
    ControlOption options[kControlMaxOptions];
};

/// The producer's answer. Not optional: an option the front-end did not
/// understand must come back as a refusal, because the alternative is a device
/// acquiring at a gain nobody prescribed.
struct ControlAck {
    std::uint32_t magic;
    std::uint16_t version;
    /// 0 accepted; otherwise 1 + the index of the first rejected option.
    std::uint16_t rejectedIndex;
    char detail[kControlDetailSize];
};
#pragma pack(pop)

static_assert(sizeof(ControlOption) == 56, "AMP control option layout is ABI");
static_assert(sizeof(ControlMessage) == 464, "AMP control message layout is ABI");
static_assert(sizeof(ControlAck) == 72, "AMP control ack layout is ABI");

std::uint32_t crc32(const void* data, std::size_t length) noexcept;

}  // namespace amp

}  // namespace med
