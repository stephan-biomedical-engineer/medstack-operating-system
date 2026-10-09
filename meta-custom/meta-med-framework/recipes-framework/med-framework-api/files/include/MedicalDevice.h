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
#include "med_amp_abi.h"

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

/// Electrode contact over one frame, one bit per channel input (bit c is
/// channel c, first sixteen channels only).
///
/// An "off" bit means something only where its "monitored" bit is set. A
/// front-end that does not check contact - a simulator, a link with no way to
/// carry the status - reports nothing monitored, and that is not the same
/// statement as "every electrode is attached". Treating a clear "off" as
/// "attached" without looking at "monitored" is the defect this type exists to
/// make hard to write; attached() does the comparison.
struct LeadOff {
    std::uint16_t monitoredPositive = 0;
    std::uint16_t monitoredNegative = 0;
    std::uint16_t offPositive = 0;
    std::uint16_t offNegative = 0;

    bool monitored() const noexcept {
        return monitoredPositive != 0 || monitoredNegative != 0;
    }
    /// Electrodes that are both monitored and reported off.
    std::uint16_t detachedPositive() const noexcept {
        return static_cast<std::uint16_t>(offPositive & monitoredPositive);
    }
    std::uint16_t detachedNegative() const noexcept {
        return static_cast<std::uint16_t>(offNegative & monitoredNegative);
    }
    bool operator==(const LeadOff& other) const noexcept {
        return monitoredPositive == other.monitoredPositive &&
               monitoredNegative == other.monitoredNegative &&
               detachedPositive() == other.detachedPositive() &&
               detachedNegative() == other.detachedNegative();
    }
    bool operator!=(const LeadOff& other) const noexcept { return !(*this == other); }
};

/// One block of samples from every channel, channel-interleaved:
/// samples[i * channelCount + c] is sample i of channel c.
struct SampleFrame {
    std::uint64_t sequence = 0;
    Timestamp timestamp{};
    std::uint16_t channelCount = 0;
    std::uint32_t samplesPerChannel = 0;
    std::vector<float> samples;
    /// Nothing monitored unless the driver's front-end checks contact.
    LeadOff leadOff;

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

/// Wire format of the AMP link. The firmware on the real-time core emits a
/// FrameHeader followed by `channelCount * samplesPerChannel` little-endian
/// int32 samples; the "rpmsg" driver converts them with scaleNanoUnitsPerLsb.
///
/// The layout is not defined here. It lives in med_amp_abi.h, a C99
/// freestanding header the firmware includes too, with the size and the offset
/// of every field asserted at compile time - one definition for two compilers,
/// two architectures and two languages. What follows only gives those types and
/// constants their C++ names, so that nothing on this side spells the C ones.
namespace amp {

constexpr std::uint32_t kFrameMagic = MED_AMP_FRAME_MAGIC;
constexpr std::uint16_t kFrameVersion = MED_AMP_FRAME_VERSION;

using FrameHeader = ::med_amp_frame_header;
using LeadOffBlock = ::med_amp_lead_off;

/// Serialise a frame in the wire format: header, samples as int32 nanovolts
/// (scaleNanoUnitsPerLsb = 1), then the lead-off block, with the CRC over
/// everything after the header. `out` is resized to fit.
///
/// Samples are in micro-units (SampleFrame's convention); one that does not fit
/// an int32 of nano-units (beyond +-2.147 units) is saturated, never wrapped.
void encodeFrame(const SampleFrame& frame, double sampleRateHz,
                 std::vector<unsigned char>& out);

/// Parse and verify one frame. Fails with IntegrityError on a short buffer, a
/// bad magic, a truncated payload or a CRC that does not close, and with
/// NotSupported on another version. On success the samples are in micro-units
/// and *sampleRateHz, when given, receives the header's rate.
///
/// The one decoder: the rpmsg driver and the HMI both call it, so a frame is
/// judged by the same code wherever it is read.
Result<SampleFrame> decodeFrame(const void* data, std::size_t length,
                                double* sampleRateHz = nullptr);

/// Control channel: the settings the application prescribes and the front-end
/// has to program, and the producer's mandatory answer. See med_amp_abi.h for
/// why it is a list of opaque strings and why it holds at most 8 options.
constexpr std::uint32_t kControlMagic = MED_AMP_CONTROL_MAGIC;
constexpr std::uint32_t kControlAckMagic = MED_AMP_CONTROL_ACK_MAGIC;
constexpr std::uint16_t kControlVersion = MED_AMP_CONTROL_VERSION;

constexpr std::uint16_t kControlMaxOptions = MED_AMP_CONTROL_MAX_OPTIONS;
constexpr std::size_t kControlKeySize = MED_AMP_CONTROL_KEY_SIZE;
constexpr std::size_t kControlValueSize = MED_AMP_CONTROL_VALUE_SIZE;
constexpr std::size_t kControlDetailSize = MED_AMP_CONTROL_DETAIL_SIZE;

using ControlOption = ::med_amp_control_option;
using ControlMessage = ::med_amp_control_message;
using ControlAck = ::med_amp_control_ack;

/// Table-driven, and deliberately a second implementation of the
/// med_amp_crc32 the firmware uses: tests/framework holds the two equal on the
/// standard check vector and on frame-sized buffers.
std::uint32_t crc32(const void* data, std::size_t length) noexcept;

}  // namespace amp

}  // namespace med
