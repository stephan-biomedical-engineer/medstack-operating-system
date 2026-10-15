// SPDX-License-Identifier: MIT
//
// The "iio" driver, against a stand-in for the kernel.
//
// Until 2026-10-14 nothing short of a board reached this driver: it reads
// /sys and a character device by absolute path. The library now takes the
// directory the kernel lists its devices in as a build-time constant, and this
// suite compiles it pointing at a tree of plain files it builds itself
// (MED_IIO_DEVICES_DIR in the Makefile) - attributes as text files, the buffer
// as a regular file full of scans. The shipped library never redefines it.
//
// What a tree of plain files can and cannot stand for is the boundary of this
// suite. It can: which attributes the driver writes and in what form, how it
// reads them back, how it lays out and decodes a scan, what it refuses. It
// cannot: a kernel attribute that refuses a value or programs a neighbour -
// those are imitated below with a file the driver may not write and one that
// reads back empty - nor poll() on a buffer that has not filled yet, nor
// anything about timing. Those stay the board's.
//
// docs/implementation_plan_usb_parity.md, phase 0.b.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "MedicalDevice.h"
#include "check.h"
#include "temp_dir.h"

using namespace med;

namespace {

const std::string kRoot = MED_TEST_IIO_ROOT;
const std::string kDir = std::string(MED_IIO_DEVICES_DIR) + "/iio:device0";
const std::string kNode = kRoot + "/dev/iio:device0";

// One step of a 24-bit converter at +-187500 uV, and the text a kernel prints
// for it in millivolts - the five significant digits that put the record off
// the converter's grid when they were used as the multiplier (BRINGUP_AFE.md
// section 6.8).
const double kFullScaleUv = 187500.0;
const double kStepUv = kFullScaleUv / 8388608.0;
const char* const kScaleText = "0.000022351";

void mkdirs(const std::string& path) {
    std::string partial;
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            partial = path.substr(0, i);
            ::mkdir(partial.c_str(), 0755);
        }
    }
}

void put(const std::string& relative, const std::string& value) {
    const std::string path = kDir + "/" + relative;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const std::string line = value + "\n";
        (void)::write(fd, line.data(), line.size());
        ::close(fd);
    }
}

std::string get(const std::string& relative) {
    const std::string path = kDir + "/" + relative;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return "<absent>";
    }
    char buffer[128];
    const ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    std::string out(buffer, n > 0 ? static_cast<std::size_t>(n) : 0U);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

/// A device the way a kernel driver of this class lists one: eight channels of
/// 24 bits right-aligned in 32, a timestamp, a gain and one private attribute.
void makeTree(const std::string& sampleType = "le:s24/32>>0") {
    medtest::removeTree(kRoot);
    mkdirs(kDir + "/scan_elements");
    mkdirs(kDir + "/buffer");
    mkdirs(kRoot + "/dev");
    put("name", "afe-under-test");
    for (int c = 0; c < 8; ++c) {
        const std::string n = std::to_string(c);
        put("in_voltage" + n + "_scale", kScaleText);
        put("scan_elements/in_voltage" + n + "_en", "0");
        put("scan_elements/in_voltage" + n + "_type", sampleType);
    }
    put("scan_elements/in_timestamp_en", "0");
    put("buffer/enable", "0");
    put("buffer/length", "0");
    put("sampling_frequency", "250");
    put("hardwaregain", "1");
    put("input_mode", "first");
    // The buffer's character device, empty: a device with nothing converted yet.
    const int fd = ::open(kNode.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ::close(fd);
    }
}

/// The buffer: `scans` scans, each eight codes right-aligned in 32 bits with
/// the upper byte zero (the kernel does not sign-extend) and a timestamp in
/// nanoseconds at the next multiple of 8.
void writeScans(const std::vector<std::vector<std::int32_t>>& scans, std::int64_t firstNs,
                std::int64_t periodNs) {
    std::vector<unsigned char> bytes;
    for (std::size_t s = 0; s < scans.size(); ++s) {
        unsigned char scan[40] = {};
        for (int c = 0; c < 8; ++c) {
            const std::uint32_t raw = static_cast<std::uint32_t>(scans[s][static_cast<std::size_t>(c)]) &
                                      0x00FFFFFFU;
            std::memcpy(scan + c * 4, &raw, 4);
        }
        const std::int64_t ts = firstNs + static_cast<std::int64_t>(s) * periodNs;
        std::memcpy(scan + 32, &ts, 8);
        bytes.insert(bytes.end(), scan, scan + 40);
    }
    const int fd = ::open(kNode.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)::write(fd, bytes.data(), bytes.size());
        ::close(fd);
    }
}

/// The same device with electrode contact: the lead-off word after the
/// voltages, and the electrodes it speaks for.
void addLeadOff(const std::string& monitored) {
    put("scan_elements/in_voltage8_lead_off_en", "0");
    put("scan_elements/in_voltage8_lead_off_type", "le:u16/32>>0");
    put("lead_off_monitored", monitored);
}

/// Scans with the lead-off word: eight voltages, the word at 32, the timestamp
/// at the next multiple of 8 - 48 bytes, the way the kernel packs them.
void writeScansLeadOff(const std::vector<std::uint32_t>& words, std::int64_t firstNs) {
    std::vector<unsigned char> bytes;
    for (std::size_t s = 0; s < words.size(); ++s) {
        unsigned char scan[48] = {};
        for (int c = 0; c < 8; ++c) {
            const std::uint32_t raw = static_cast<std::uint32_t>(100 * (c + 1));
            std::memcpy(scan + c * 4, &raw, 4);
        }
        std::memcpy(scan + 32, &words[s], 4);
        const std::int64_t ts = firstNs + static_cast<std::int64_t>(s) * 4000000;
        std::memcpy(scan + 40, &ts, 8);
        bytes.insert(bytes.end(), scan, scan + 48);
    }
    const int fd = ::open(kNode.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)::write(fd, bytes.data(), bytes.size());
        ::close(fd);
    }
}

DeviceConfig iioConfig(const std::map<std::string, std::string>& options) {
    DeviceConfig config;
    config.driver = "iio";
    config.id = "eeg0";
    config.address = kNode;
    config.channelCount = 8;
    config.sampleRateHz = 250.0;
    config.samplesPerFrame = 14;
    config.driverOptions = options;
    return config;
}

std::map<std::string, std::string> declared() {
    return {{"full_scale", "187500"}, {"resolution_bits", "24"}, {"hardwaregain", "24"},
            {"input_mode", "second"}};
}

std::unique_ptr<MedicalDevice> make(const std::map<std::string, std::string>& options) {
    Result<std::unique_ptr<MedicalDevice>> created =
        MedicalDeviceFactory::create(iioConfig(options));
    return created.isOk() ? created.take() : nullptr;
}

}  // namespace

void testIio() {
    medtest::section("iio - a partida escreve o que a configuração pede, e o lê de volta");
    {
        makeTree();
        writeScans({}, 0, 4000000);
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->selfTest(), Status::Ok);
            CHECK_STATUS(device->start(), Status::Ok);
            CHECK_EQ(get("hardwaregain"), std::string("24"));
            CHECK_EQ(get("input_mode"), std::string("second"));
            // The rate comes from the device's configuration, never an option.
            CHECK_EQ(get("sampling_frequency"), std::string("250"));
            bool allEnabled = true;
            for (int c = 0; c < 8; ++c) {
                allEnabled = allEnabled &&
                             get("scan_elements/in_voltage" + std::to_string(c) + "_en") == "1";
            }
            CHECK_MSG(allEnabled, "um canal ficou fora do buffer");
            CHECK_EQ(get("scan_elements/in_timestamp_en"), std::string("1"));
            // Two frames of headroom: 2 x 14 samples x 8 channels.
            CHECK_EQ(get("buffer/length"), std::string("224"));
            CHECK_EQ(get("buffer/enable"), std::string("1"));
            // Never written: the two declarations are not attributes.
            CHECK_EQ(get("full_scale"), std::string("<absent>"));
            device->stop();
            CHECK_EQ(get("buffer/enable"), std::string("0"));
        }
    }

    medtest::section("iio - cada código vira microvolts pelo passo declarado, com sinal");
    {
        makeTree();
        // Two frames. Channel c of scan s carries a code chosen so that the
        // edges of the range and the sign extension are all exercised.
        const std::int32_t edges[] = {0, 1, -1, 8388607, -8388608, 123456, -654321, 42};
        std::vector<std::vector<std::int32_t>> scans;
        for (int s = 0; s < 28; ++s) {
            std::vector<std::int32_t> scan(8);
            for (int c = 0; c < 8; ++c) {
                scan[static_cast<std::size_t>(c)] = (s == 0) ? edges[c] : (s * 1000 + c) * (c % 2 ? -1 : 1);
            }
            scans.push_back(scan);
        }
        const std::int64_t firstNs = 1700000000123456789LL;
        writeScans(scans, firstNs, 4000000);

        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device && device->start() == Status::Ok) {
            Result<SampleFrame> first = device->read(std::chrono::milliseconds(100));
            CHECK(first.isOk());
            if (first.isOk()) {
                const SampleFrame& frame = first.value();
                CHECK_EQ(frame.sequence, static_cast<std::uint64_t>(1));
                CHECK_EQ(frame.channelCount, static_cast<std::uint16_t>(8));
                CHECK_EQ(frame.samplesPerChannel, static_cast<std::uint32_t>(14));
                bool exact = true;
                for (int c = 0; c < 8; ++c) {
                    exact = exact && frame.samples[static_cast<std::size_t>(c)] ==
                                         static_cast<double>(edges[c]) * kStepUv;
                }
                CHECK_MSG(exact, "um código das bordas não virou código x passo");
                // The step is the declared one, not the kernel's text: a full
                // scale code lands on full scale to the last bit.
                CHECK(frame.samples[4] == -kFullScaleUv);
                // The frame is stamped with its FIRST scan's time.
                CHECK_EQ(toUnixMicros(frame.timestamp),
                         static_cast<std::uint64_t>(firstNs / 1000));
            }
            Result<SampleFrame> second = device->read(std::chrono::milliseconds(100));
            CHECK(second.isOk());
            if (second.isOk()) {
                CHECK_EQ(second.value().sequence, static_cast<std::uint64_t>(2));
                // Scan 14, channel 3: (14 * 1000 + 3) * -1.
                CHECK(second.value().samples[3] == -14003.0 * kStepUv);
            }
            // No more scans and nobody writing: the front-end closed.
            Result<SampleFrame> third = device->read(std::chrono::milliseconds(50));
            CHECK_STATUS(third.status(), Status::Unavailable);
            device->stop();
        }
    }

    medtest::section("iio - o que a partida recusa, e por quê");
    {
        // An option that is not an attribute of this device.
        makeTree();
        std::map<std::string, std::string> options = declared();
        options["no_such_attribute"] = "1";
        std::unique_ptr<MedicalDevice> device = make(options);
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::NotSupported);
            CHECK_MSG(device->lastError().find("no_such_attribute") != std::string::npos,
                      device->lastError());
        }
    }
    if (::geteuid() != 0) {
        // An attribute that refuses the value (here: one the driver may not
        // write, which is what a refusing kernel attribute looks like from
        // userspace - the write fails).
        makeTree();
        ::chmod((kDir + "/input_mode").c_str(), 0444);
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::OutOfRange);
            CHECK_MSG(device->lastError().find("refused input_mode") != std::string::npos,
                      device->lastError());
        }
    } else {
        std::printf("  (como root: o atributo somente-leitura não recusa nada; caso pulado)\n");
    }
    {
        // An attribute that takes the value and programs another one: what is
        // read back is not what was written.
        makeTree();
        ::unlink((kDir + "/hardwaregain").c_str());
        ::symlink("/dev/null", (kDir + "/hardwaregain").c_str());
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::OutOfRange);
            CHECK_MSG(device->lastError().find("programmed") != std::string::npos,
                      device->lastError());
        }
    }
    {
        // A declaration the kernel's scale contradicts: the configuration
        // says a full scale of twice what the device's gain gives.
        makeTree();
        std::map<std::string, std::string> options = declared();
        options["full_scale"] = "375000";
        std::unique_ptr<MedicalDevice> device = make(options);
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::IntegrityError);
        }
    }
    {
        // A sample format this driver does not decode is refused, not misread.
        makeTree("le:s16/16>>0");
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::NotSupported);
            CHECK_EQ(get("buffer/enable"), std::string("0"));
        }
    }
    {
        // A configuration that declares 24 bits on a device that converts 16
        // in 32-bit storage.
        makeTree("le:s16/32>>0");
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::IntegrityError);
        }
    }
    {
        // The rate is not an option: one setting, one source.
        makeTree();
        std::map<std::string, std::string> options = declared();
        options["sampling_frequency"] = "500";
        CHECK(!MedicalDeviceFactory::create(iioConfig(options)).isOk());
        // Nor is a path: an option reaches this device's own directory only.
        options = declared();
        options["../name"] = "x";
        CHECK(!MedicalDeviceFactory::create(iioConfig(options)).isOk());
    }

    medtest::section("iio - o contato dos eletrodos, quadro a quadro");
    {
        // A front-end without the lead-off word: nothing monitored, which is
        // not the same as every electrode attached.
        makeTree();
        writeScans(std::vector<std::vector<std::int32_t>>(14, std::vector<std::int32_t>(8, 0)), 0,
                   4000000);
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device && device->start() == Status::Ok) {
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(100));
            CHECK(frame.isOk());
            if (frame.isOk()) {
                CHECK(!frame.value().leadOff.monitored());
            }
            device->stop();
        }
    }
    {
        // Bipolar, every electrode watched. One sample of the frame says 3+
        // and 5- are off: the frame says so, and the voltages are untouched.
        makeTree();
        addLeadOff("ff ff");
        std::vector<std::uint32_t> words(28, 0);
        words[6] = (0x04U << 8) | 0x10U;
        writeScansLeadOff(words, 1000000000LL);
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device && device->start() == Status::Ok) {
            CHECK_EQ(get("scan_elements/in_voltage8_lead_off_en"), std::string("1"));
            Result<SampleFrame> first = device->read(std::chrono::milliseconds(100));
            CHECK(first.isOk());
            if (first.isOk()) {
                const LeadOff& contact = first.value().leadOff;
                CHECK_EQ(contact.monitoredPositive, static_cast<std::uint16_t>(0xff));
                CHECK_EQ(contact.monitoredNegative, static_cast<std::uint16_t>(0xff));
                CHECK_EQ(contact.detachedPositive(), static_cast<std::uint16_t>(0x04));
                CHECK_EQ(contact.detachedNegative(), static_cast<std::uint16_t>(0x10));
                // 100 codes on channel 0: the word did not shift the voltages.
                CHECK(first.value().samples[0] == 100.0 * kStepUv);
                CHECK(first.value().samples[7] == 800.0 * kStepUv);
                CHECK_EQ(toUnixMicros(first.value().timestamp), static_cast<std::uint64_t>(1000000));
            }
            Result<SampleFrame> second = device->read(std::chrono::milliseconds(100));
            CHECK(second.isOk());
            if (second.isOk()) {
                CHECK(second.value().leadOff.monitored());
                CHECK_EQ(second.value().leadOff.detachedPositive(), static_cast<std::uint16_t>(0));
            }
            device->stop();
        }
    }
    {
        // Referential: one source on the negative side. A negative bit other
        // than the reference electrode's is not a measurement, whatever the
        // word carries.
        makeTree();
        addLeadOff("ff 01");
        std::vector<std::uint32_t> words(14, 0);
        words[0] = 0x10U;
        words[1] = 0x01U;
        writeScansLeadOff(words, 0);
        std::unique_ptr<MedicalDevice> device = make(declared());
        if (device && device->start() == Status::Ok) {
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(100));
            CHECK(frame.isOk());
            if (frame.isOk()) {
                CHECK_EQ(frame.value().leadOff.monitoredNegative, static_cast<std::uint16_t>(0x01));
                CHECK_EQ(frame.value().leadOff.detachedNegative(), static_cast<std::uint16_t>(0x01));
            }
            device->stop();
        }
    }
    {
        // A word and no list of what it watches: refused, not guessed.
        makeTree();
        addLeadOff("ff ff");
        ::unlink((kDir + "/lead_off_monitored").c_str());
        std::unique_ptr<MedicalDevice> device = make(declared());
        if (device) {
            CHECK_STATUS(device->start(), Status::NotSupported);
            CHECK_EQ(get("buffer/enable"), std::string("0"));
        }
        // A word of another shape: refused, not misread.
        makeTree();
        addLeadOff("ff ff");
        put("scan_elements/in_voltage8_lead_off_type", "le:u8/8>>0");
        device = make(declared());
        if (device) {
            CHECK_STATUS(device->start(), Status::NotSupported);
        }
    }

    medtest::section("iio - uma perda contada pelo kernel deixa marca na sequência");
    {
        // Three frames of scans; scan s carries code s on every channel, so a
        // frame says by its first sample which scans it holds.
        makeTree();
        put("lost_samples", "0");
        std::vector<std::vector<std::int32_t>> scans;
        for (int s = 0; s < 42; ++s) {
            scans.push_back(std::vector<std::int32_t>(8, s));
        }
        writeScans(scans, 0, 4000000);
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device && device->start() == Status::Ok) {
            Result<SampleFrame> first = device->read(std::chrono::milliseconds(100));
            CHECK(first.isOk());
            if (first.isOk()) {
                CHECK_EQ(first.value().sequence, static_cast<std::uint64_t>(1));
                CHECK_EQ(first.value().samples[0], 0.0);
            }
            // The kernel counted a conversion it could not deliver while the
            // second frame filled: that frame is given up and its number
            // skipped, and the next one is whole.
            put("lost_samples", "1");
            Result<SampleFrame> next = device->read(std::chrono::milliseconds(100));
            CHECK(next.isOk());
            if (next.isOk()) {
                CHECK_EQ(next.value().sequence, static_cast<std::uint64_t>(3));
                CHECK_EQ(next.value().samples[0], 28.0 * kStepUv);
            }
            device->stop();
        }
    }
    {
        // A count that rose before the first frame: the first frame goes.
        makeTree();
        put("lost_samples", "0");
        std::vector<std::vector<std::int32_t>> scans;
        for (int s = 0; s < 42; ++s) {
            scans.push_back(std::vector<std::int32_t>(8, s));
        }
        writeScans(scans, 0, 4000000);
        std::unique_ptr<MedicalDevice> device = make(declared());
        if (device && device->start() == Status::Ok) {
            put("lost_samples", "3");
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(100));
            CHECK(frame.isOk());
            if (frame.isOk()) {
                CHECK_EQ(frame.value().sequence, static_cast<std::uint64_t>(2));
                CHECK_EQ(frame.value().samples[0], 14.0 * kStepUv);
            }
            // A count that is not a number is not a reason to guess.
            put("lost_samples", "3 lost");
            CHECK_STATUS(device->read(std::chrono::milliseconds(100)).status(), Status::IoError);
            device->stop();
        }
        // Unreadable at the start: the device does not start.
        makeTree();
        put("lost_samples", "many");
        writeScans(scans, 0, 4000000);
        device = make(declared());
        if (device) {
            CHECK_STATUS(device->start(), Status::IoError);
            CHECK_EQ(get("buffer/enable"), std::string("0"));
        }
    }

    medtest::section("iio - um canal com passo próprio (o sensor a outro ganho)");
    {
        // Channel 7 runs at half the gain of the rest: twice the step. The
        // kernel says so in its scale; the configuration declares it.
        const double step7 = 2.0 * kStepUv;
        makeTree();
        put("in_voltage7_scale", "0.000044703");
        std::vector<std::vector<std::int32_t>> scans(14, std::vector<std::int32_t>(8, 1000));
        writeScans(scans, 0, 4000000);
        std::map<std::string, std::string> options = declared();
        options["full_scale.7"] = "375000";
        std::unique_ptr<MedicalDevice> device = make(options);
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->start(), Status::Ok);
            // Not an attribute, never written.
            CHECK_EQ(get("full_scale.7"), std::string("<absent>"));
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(100));
            CHECK(frame.isOk());
            if (frame.isOk()) {
                CHECK(frame.value().samples[0] == 1000.0 * kStepUv);
                CHECK(frame.value().samples[6] == 1000.0 * kStepUv);
                CHECK(frame.value().samples[7] == 1000.0 * step7);
            }
            device->stop();
        }
    }
    {
        // The kernel runs channel 7 at another gain and the configuration
        // does not say so: refused, naming the channel - the defect a single
        // scale for all eight channels used to let through, scaled by 2.
        makeTree();
        put("in_voltage7_scale", "0.000044703");
        writeScans({}, 0, 4000000);
        std::unique_ptr<MedicalDevice> device = make(declared());
        if (device) {
            CHECK_STATUS(device->start(), Status::IntegrityError);
            CHECK(device->lastError().find("channel 7") != std::string::npos);
        }
        // And the other way: declared and not what the kernel runs.
        makeTree();
        writeScans({}, 0, 4000000);
        std::map<std::string, std::string> options = declared();
        options["full_scale.7"] = "375000";
        device = make(options);
        if (device) {
            CHECK_STATUS(device->start(), Status::IntegrityError);
        }
    }
    {
        // With no declaration at all, each channel takes its own kernel text.
        makeTree();
        put("in_voltage7_scale", "0.000044703");
        std::vector<std::vector<std::int32_t>> scans(14, std::vector<std::int32_t>(8, 1000));
        writeScans(scans, 0, 4000000);
        std::unique_ptr<MedicalDevice> device = make({{"hardwaregain", "24"}});
        if (device && device->start() == Status::Ok) {
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(100));
            CHECK(frame.isOk());
            if (frame.isOk()) {
                // Each by its own text, millivolts to microvolts - which is
                // why the text is not exactly twice the other, and why a
                // declared step is the better multiplier.
                CHECK(std::fabs(frame.value().samples[0] - 1000.0 * 0.022351) < 1e-9);
                CHECK(std::fabs(frame.value().samples[7] - 1000.0 * 0.044703) < 1e-9);
            }
            device->stop();
        }
    }
    {
        // A channel's range that names no channel, or stands alone.
        std::map<std::string, std::string> options = declared();
        options["full_scale.8"] = "375000";
        CHECK(make(options) == nullptr);
        options = declared();
        options["full_scale.x"] = "375000";
        CHECK(make(options) == nullptr);
        options = declared();
        options["full_scale.7"] = "-1";
        CHECK(make(options) == nullptr);
        CHECK(make({{"full_scale.7", "375000"}}) == nullptr);
    }

    medtest::section("iio - o autoteste diz o que falta");
    {
        makeTree();
        ::unlink((kDir + "/in_voltage7_scale").c_str());
        std::unique_ptr<MedicalDevice> device = make(declared());
        CHECK(device != nullptr);
        if (device) {
            CHECK_STATUS(device->selfTest(), Status::OutOfRange);
        }
        makeTree();
        put("name", "");
        device = make(declared());
        if (device) {
            CHECK_STATUS(device->selfTest(), Status::Unavailable);
        }
        // No device at the address at all.
        medtest::removeTree(kRoot);
        device = make(declared());
        if (device) {
            CHECK_STATUS(device->selfTest(), Status::NotFound);
        }
    }
    medtest::removeTree(kRoot);
}
