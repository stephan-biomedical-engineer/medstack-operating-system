// SPDX-License-Identifier: MIT
//
// med_amp_abi.h: the AMP wire format, held to one layout across compilers.
//
// test_device.cpp checks the sizes. This file checks what a size cannot: the
// offset of every field, as the C++ compiler sees it, against an independent
// table and against what a C compiler computes from the same header
// (amp_abi_c_probe.c, built as C99 and linked in). Two fields of the same
// width swapped keep every size and change the format; a CRC computed over the
// result still closes.
//
// The expected numbers below come from implementation_plan_m33_firmware.md
// §6.3 and §6.2, which were written from the layout before it moved out of
// MedicalDevice.h. That makes them a record of the format the "rpmsg" driver
// was already speaking, not a second reading of the new header.
//
// What this file cannot see: how the Cortex-M33 toolchain packs the header.
// The Makefile compiles amp_abi_c_probe.c with arm-none-eabi-gcc for that
// core, where the header's own static assertions are the test.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "MedicalDevice.h"
#include "check.h"

extern "C" {
extern const std::size_t med_amp_c_layout[];
extern const std::size_t med_amp_c_layout_count;
std::uint32_t med_amp_c_crc32(const void* data, std::size_t length);
}

using namespace med;

namespace {

struct Field {
    const char* name;
    std::size_t cxx;       // what the C++ compiler computes
    std::size_t expected;  // what the format is
};

#define MED_FIELD(type, member, at) {#type "." #member, offsetof(type, member), at}

// Same order as med_amp_c_layout in amp_abi_c_probe.c.
const Field kLayout[] = {
    {"sizeof FrameHeader", sizeof(amp::FrameHeader), 40},
    MED_FIELD(amp::FrameHeader, magic, 0),
    MED_FIELD(amp::FrameHeader, version, 4),
    MED_FIELD(amp::FrameHeader, channelCount, 6),
    MED_FIELD(amp::FrameHeader, samplesPerChannel, 8),
    MED_FIELD(amp::FrameHeader, sampleRateMilliHz, 12),
    MED_FIELD(amp::FrameHeader, sequence, 16),
    MED_FIELD(amp::FrameHeader, timestampMicros, 24),
    MED_FIELD(amp::FrameHeader, scaleNanoUnitsPerLsb, 32),
    MED_FIELD(amp::FrameHeader, crc32, 36),

    // Version 2's lead-off block. Expected values from the header's own
    // description (four uint16, packed), not from a plan.
    {"sizeof LeadOffBlock", sizeof(amp::LeadOffBlock), 8},
    MED_FIELD(amp::LeadOffBlock, monitoredPositive, 0),
    MED_FIELD(amp::LeadOffBlock, monitoredNegative, 2),
    MED_FIELD(amp::LeadOffBlock, offPositive, 4),
    MED_FIELD(amp::LeadOffBlock, offNegative, 6),

    {"sizeof ControlOption", sizeof(amp::ControlOption), 56},
    MED_FIELD(amp::ControlOption, key, 0),
    MED_FIELD(amp::ControlOption, value, 32),

    {"sizeof ControlMessage", sizeof(amp::ControlMessage), 464},
    MED_FIELD(amp::ControlMessage, magic, 0),
    MED_FIELD(amp::ControlMessage, version, 4),
    MED_FIELD(amp::ControlMessage, optionCount, 6),
    MED_FIELD(amp::ControlMessage, crc32, 8),
    MED_FIELD(amp::ControlMessage, reserved, 12),
    MED_FIELD(amp::ControlMessage, options, 16),

    {"sizeof ControlAck", sizeof(amp::ControlAck), 72},
    MED_FIELD(amp::ControlAck, magic, 0),
    MED_FIELD(amp::ControlAck, version, 4),
    MED_FIELD(amp::ControlAck, rejectedIndex, 6),
    MED_FIELD(amp::ControlAck, detail, 8),
};

#undef MED_FIELD

constexpr std::size_t kLayoutCount = sizeof(kLayout) / sizeof(kLayout[0]);

}  // namespace

void testAmpAbi() {
    medtest::section("ABI do enlace AMP: um cabeçalho C, dois compiladores");

    // The C++ names are the C types, not copies of them. A struct redefined in
    // MedicalDevice.h would compile, pass every size check and be free to drift.
    CHECK((std::is_same<amp::FrameHeader, med_amp_frame_header>::value));
    CHECK((std::is_same<amp::ControlOption, med_amp_control_option>::value));
    CHECK((std::is_same<amp::ControlMessage, med_amp_control_message>::value));
    CHECK((std::is_same<amp::ControlAck, med_amp_control_ack>::value));
    CHECK((std::is_same<amp::LeadOffBlock, med_amp_lead_off>::value));

    // memcpy onto the wire is only defined for trivially copyable types.
    CHECK(std::is_trivially_copyable<amp::FrameHeader>::value);
    CHECK(std::is_trivially_copyable<amp::ControlMessage>::value);
    CHECK(std::is_trivially_copyable<amp::ControlAck>::value);

    // The table has to describe every field the C side exports, or a field
    // added to one list and not the other would compare the rest out of step.
    CHECK_EQ(static_cast<long long>(med_amp_c_layout_count),
             static_cast<long long>(kLayoutCount));

    medtest::section("ABI do enlace AMP: offset de cada campo");

    const std::size_t count =
        med_amp_c_layout_count < kLayoutCount ? med_amp_c_layout_count : kLayoutCount;
    for (std::size_t i = 0; i < count; ++i) {
        const Field& f = kLayout[i];
        CHECK_MSG(f.cxx == f.expected,
                  std::string(f.name) + ": C++ diz " + std::to_string(f.cxx) +
                      ", o formato diz " + std::to_string(f.expected));
        CHECK_MSG(med_amp_c_layout[i] == f.cxx,
                  std::string(f.name) + ": C diz " + std::to_string(med_amp_c_layout[i]) +
                      ", C++ diz " + std::to_string(f.cxx));
    }

    medtest::section("ABI do enlace AMP: três CRC-32, um resultado");

    // The framework's table-driven CRC, the header's bitwise one compiled as
    // C++, and the same compiled as C. The firmware will use the third.
    {
        // The published check value of CRC-32/IEEE 802.3 for "123456789" - the
        // one vector here whose source is outside this repository.
        const char* const check = "123456789";
        CHECK_EQ(static_cast<long long>(med_amp_crc32(check, 9)), 0xCBF43926LL);
        CHECK_EQ(static_cast<long long>(med_amp_c_crc32(check, 9)), 0xCBF43926LL);
        CHECK_EQ(static_cast<long long>(amp::crc32(check, 9)), 0xCBF43926LL);

        CHECK_EQ(static_cast<long long>(med_amp_c_crc32("", 0)), 0LL);
    }

    {
        // A frame-sized payload with every byte value in it, then every single
        // bit of it flipped in turn: the three must agree on all 3585 inputs,
        // and every flip must change the result.
        unsigned char payload[448];
        std::uint32_t state = 0x12345678U;
        for (unsigned char& byte : payload) {
            state = state * 1664525U + 1013904223U;
            byte = static_cast<unsigned char>(state >> 24);
        }

        const std::uint32_t clean = amp::crc32(payload, sizeof(payload));
        int disagreements = 0;
        int unchanged = 0;
        auto agree = [&](const unsigned char* data) {
            const std::uint32_t a = amp::crc32(data, sizeof(payload));
            if (a != med_amp_crc32(data, sizeof(payload)) ||
                a != med_amp_c_crc32(data, sizeof(payload))) {
                ++disagreements;
            }
            return a;
        };
        agree(payload);
        for (std::size_t bit = 0; bit < sizeof(payload) * 8; ++bit) {
            payload[bit / 8] = static_cast<unsigned char>(payload[bit / 8] ^ (1U << (bit % 8)));
            if (agree(payload) == clean) {
                ++unchanged;
            }
            payload[bit / 8] = static_cast<unsigned char>(payload[bit / 8] ^ (1U << (bit % 8)));
        }
        CHECK_EQ(disagreements, 0);
        CHECK_EQ(unchanged, 0);
    }

    medtest::section("ABI do enlace AMP: codificar e decodificar um quadro v2");

    SampleFrame frame;
    frame.sequence = 41;
    frame.timestamp = fromUnixMicros(1234567);
    frame.channelCount = 8;
    frame.samplesPerChannel = 14;
    for (std::size_t i = 0; i < 8 * 14; ++i) {
        // Micro-units with a fraction a float carries exactly to the nanovolt.
        frame.samples.push_back(static_cast<float>(static_cast<int>(i) - 50) * 0.5F);
    }
    frame.leadOff.monitoredPositive = 0x00FF;
    frame.leadOff.monitoredNegative = 0x00FF;
    frame.leadOff.offPositive = 0x0005;
    frame.leadOff.offNegative = 0x0080;

    std::vector<unsigned char> wire;
    amp::encodeFrame(frame, 250.0, wire);
    // The producer's geometry is the whole rpmsg payload (med_amp_abi.h).
    CHECK_EQ(static_cast<long long>(wire.size()), 496LL);
    {
        amp::FrameHeader header;
        std::memcpy(&header, wire.data(), sizeof(header));
        CHECK_EQ(static_cast<long long>(header.version), 2LL);
        CHECK_EQ(static_cast<long long>(header.crc32),
                 static_cast<long long>(amp::crc32(wire.data() + sizeof(header),
                                                   wire.size() - sizeof(header))));
    }

    double rate = 0.0;
    Result<SampleFrame> decoded = amp::decodeFrame(wire.data(), wire.size(), &rate);
    CHECK(decoded.isOk());
    if (decoded.isOk()) {
        const SampleFrame& d = decoded.value();
        CHECK_EQ(static_cast<long long>(d.sequence), 41LL);
        CHECK_EQ(static_cast<long long>(toUnixMicros(d.timestamp)), 1234567LL);
        CHECK(d.samples == frame.samples);
        CHECK(d.leadOff == frame.leadOff);
        CHECK_EQ(static_cast<long long>(d.leadOff.offPositive), 0x0005LL);
        CHECK_EQ(static_cast<long long>(d.leadOff.offNegative), 0x0080LL);
        CHECK(rate == 250.0);
    }

    // Every way a frame can be wrong, each on a fresh copy.
    {
        std::vector<unsigned char> bad = wire;
        bad.back() = static_cast<unsigned char>(bad.back() ^ 0x01U);  // in the lead-off block
        Result<SampleFrame> r = amp::decodeFrame(bad.data(), bad.size());
        CHECK_MSG(!r.isOk() && r.status() == Status::IntegrityError,
                  "um bit trocado no bloco de lead-off passou: o CRC não o cobre");
    }
    {
        Result<SampleFrame> r = amp::decodeFrame(wire.data(), wire.size() - 1);
        CHECK_MSG(!r.isOk() && r.status() == Status::IntegrityError,
                  "quadro sem o último byte do bloco de lead-off foi aceito");
    }
    {
        std::vector<unsigned char> v1 = wire;
        const std::uint16_t one = 1;
        std::memcpy(v1.data() + offsetof(amp::FrameHeader, version), &one, sizeof(one));
        Result<SampleFrame> r = amp::decodeFrame(v1.data(), v1.size());
        CHECK_MSG(!r.isOk() && r.status() == Status::NotSupported,
                  "um quadro v1 foi aceito por um decodificador v2");
    }
    {
        Result<SampleFrame> r = amp::decodeFrame(nullptr, 0);
        CHECK(!r.isOk() && r.status() == Status::IntegrityError);
    }

    medtest::section("ABI do enlace AMP: o que \"monitorado\" quer dizer");
    {
        LeadOff none;
        CHECK(!none.monitored());
        // Bits off where nothing is monitored are not detachments, and two
        // statuses that differ only there are the same statement.
        LeadOff noise;
        noise.offPositive = 0xFFFF;
        CHECK_EQ(static_cast<long long>(noise.detachedPositive()), 0LL);
        CHECK(noise == none);
        LeadOff one;
        one.monitoredPositive = 0x00FF;
        one.offPositive = 0x0102;
        CHECK_EQ(static_cast<long long>(one.detachedPositive()), 0x0002LL);
        CHECK(one != none);
    }

    medtest::section("ABI do enlace AMP: saturar, nunca dar a volta");
    {
        SampleFrame big;
        big.channelCount = 1;
        big.samplesPerChannel = 2;
        big.samples = {3.0e6F, -3.0e6F};  // 3 V in microvolts: beyond int32 nV
        std::vector<unsigned char> out;
        amp::encodeFrame(big, 250.0, out);
        std::int32_t raw[2];
        std::memcpy(raw, out.data() + sizeof(amp::FrameHeader), sizeof(raw));
        CHECK_EQ(static_cast<long long>(raw[0]), 2147483647LL);
        CHECK_EQ(static_cast<long long>(raw[1]), -2147483648LL);
    }
}
