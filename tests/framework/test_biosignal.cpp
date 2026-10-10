// SPDX-License-Identifier: MIT
//
// BiosignalWriter: the BDF+ format, byte by byte.
//
// These checks are written from the same reading of the specification that
// wrote the writer, so on their own they prove consistency and not
// correctness (implementation_plan_bdf.md, phase 2). What makes them evidence
// is the second, independent reader that opens the files this suite leaves
// behind - see README.md. The two things checked here that do not depend on
// that reading are the ones the clinical record rests on: that every code
// written comes back exactly, and that the sidecar is the digest an outside
// tool computes.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "MedicalStorage.h"
#include "check.h"
#include "temp_dir.h"

using namespace med;
using medtest::TempDir;

namespace {

std::vector<unsigned char> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>());
}

/// A header field, with the padding the format adds removed.
std::string field(const std::vector<unsigned char>& file, std::size_t offset, std::size_t width) {
    if (offset + width > file.size()) {
        return "<fora do arquivo>";
    }
    std::string text(file.begin() + static_cast<std::ptrdiff_t>(offset),
                     file.begin() + static_cast<std::ptrdiff_t>(offset + width));
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

std::int32_t sample24(const std::vector<unsigned char>& file, std::size_t offset) {
    std::uint32_t raw = static_cast<std::uint32_t>(file[offset]) |
                        (static_cast<std::uint32_t>(file[offset + 1]) << 8) |
                        (static_cast<std::uint32_t>(file[offset + 2]) << 16);
    if ((raw & 0x800000u) != 0) {
        raw |= 0xFF000000u;
    }
    return static_cast<std::int32_t>(raw);
}

/// sha256sum, from coreutils: a second implementation of the digest, not the
/// one the writer used.
std::string externalSha256(const std::string& path) {
    const std::string command = "sha256sum '" + path + "' 2>/dev/null";
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return {};
    }
    char buffer[128] = {0};
    const std::size_t got = std::fread(buffer, 1, 64, pipe);
    ::pclose(pipe);
    return std::string(buffer, got);
}

/// Eight signals, 14 samples per record, 56 ms - the geometry of the AMP link -
/// with the scale of a 24-bit converter at gain 24 on a 4.5 V reference.
BiosignalHeader eightChannels(std::uint64_t records) {
    BiosignalHeader header;
    header.patient = "X X X X";
    header.recording = "2026-10-09T02:33:37.356Z X MedPlatform_eeg0_1.0.0";
    header.year = 2026;
    header.month = 10;
    header.day = 9;
    header.hour = 2;
    header.minute = 33;
    header.second = 37;
    header.startFraction = 0.356;
    header.recordDurationSeconds = 0.056;
    header.samplesPerRecord = 14;
    const double lsb = 375000.0 / 16777216.0;  // uV
    for (int c = 1; c <= 8; ++c) {
        BiosignalSignal signal;
        signal.label = "EEG " + std::to_string(c) + "+-" + std::to_string(c) + "-";
        signal.physicalDimension = "uV";
        signal.physicalMinimum = -8388608.0 * lsb;
        signal.physicalMaximum = 8388607.0 * lsb;
        signal.prefiltering = "HP:DC LP:65Hz";
        header.signals.push_back(signal);
    }
    header.annotationBytesPerRecord = 60;
    header.recordCount = records;
    return header;
}

std::vector<std::int32_t> ramp(std::int32_t base) {
    std::vector<std::int32_t> samples(14 * 8);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        // Both signs, both extremes of a 24-bit code, and something in between.
        const std::int32_t step = static_cast<std::int32_t>(i);
        samples[i] = (i % 3 == 0)   ? base + step
                     : (i % 3 == 1) ? -base - step
                                    : ((i % 2) ? 8388607 : -8388608);
    }
    return samples;
}

/// Copies a finished file out of the scratch directory when MED_BDF_SAMPLE_DIR
/// names one, for the independent reader (README.md).
void keepSample(const std::string& from, const std::string& name) {
    const char* keep = std::getenv("MED_BDF_SAMPLE_DIR");
    if (keep == nullptr) {
        return;
    }
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(std::string(keep) + "/" + name, std::ios::binary);
    out << in.rdbuf();
}

}  // namespace

void testBiosignal() {
    medtest::section("BiosignalWriter - cabeçalho");

    TempDir dir;
    CHECK(dir.valid());
    StorageConfig config;
    config.root = dir.path();
    config.nameSpace = "";
    config.requireEncryptedBacking = false;
    Result<std::unique_ptr<MedicalStorage>> opened = MedicalStorage::open(config);
    CHECK(opened.isOk());
    if (!opened) {
        return;
    }
    MedicalStorage& storage = *opened.value();

    const std::size_t ns = 9;  // eight signals and the annotation signal
    const std::size_t headerBytes = 256 * (ns + 1);
    const std::size_t recordBytes = 8 * 14 * 3 + 60;

    {
        Result<std::unique_ptr<BiosignalWriter>> made =
            BiosignalWriter::create(storage, "s/record.bdf", eightChannels(3));
        CHECK_MSG(made.isOk(), made.message());
        if (!made) {
            return;
        }
        BiosignalWriter& writer = *made.value();

        const std::vector<std::int32_t> first = ramp(1000);
        const std::vector<std::int32_t> second = ramp(-77);
        // An event "at the start" is at the first sample, startFraction, and
        // not at 0: onsets count from the header's whole second, readers
        // measure from the first sample, and MNE drops what falls before it
        // (observed, implementation_plan_bdf.md phase 2).
        CHECK_STATUS(writer.appendRecord(0.356, first.data(),
                                         {{0.356, -1.0, "montage bipolar"},
                                          {0.4, 0.056, "lead-off: 3+"}}),
                     Status::Ok);
        CHECK_STATUS(writer.appendRecord(0.412, second.data()), Status::Ok);
        CHECK_STATUS(writer.appendRecord(0.468, first.data()), Status::Ok);

        CHECK(!storage.exists("s/record.bdf.sha256"));
        CHECK_STATUS(writer.finish(), Status::Ok);
        CHECK(storage.exists("s/record.bdf.sha256"));

        const std::vector<unsigned char> file = slurp(dir.file("s/record.bdf"));
        CHECK_EQ(file.size(), headerBytes + 3 * recordBytes);

        // The general header (EDF 0.2, BDF+).
        CHECK_EQ(static_cast<int>(file[0]), 0xFF);
        CHECK_EQ(field(file, 1, 7), std::string("BIOSEMI"));
        CHECK_EQ(field(file, 8, 80), std::string("X X X X"));
        CHECK_EQ(field(file, 88, 80),
                 std::string("Startdate 09-OCT-2026 2026-10-09T02:33:37.356Z X "
                             "MedPlatform_eeg0_1.0.0"));
        CHECK_EQ(field(file, 168, 8), std::string("09.10.26"));
        CHECK_EQ(field(file, 176, 8), std::string("02.33.37"));
        CHECK_EQ(field(file, 184, 8), std::to_string(headerBytes));
        CHECK_EQ(field(file, 192, 44), std::string("BDF+C"));
        CHECK_EQ(field(file, 236, 8), std::string("3"));
        CHECK_EQ(field(file, 244, 8), std::string("0.056"));
        CHECK_EQ(field(file, 252, 4), std::string("9"));

        // Per signal, each field for all signals in turn.
        const std::size_t labels = 256;
        const std::size_t transducers = labels + ns * 16;
        const std::size_t dimensions = transducers + ns * 80;
        const std::size_t physMin = dimensions + ns * 8;
        const std::size_t physMax = physMin + ns * 8;
        const std::size_t digMin = physMax + ns * 8;
        const std::size_t digMax = digMin + ns * 8;
        const std::size_t prefilter = digMax + ns * 8;
        const std::size_t perRecord = prefilter + ns * 80;
        const std::size_t reserved = perRecord + ns * 8;
        CHECK_EQ(reserved + ns * 32, headerBytes);

        CHECK_EQ(field(file, labels, 16), std::string("EEG 1+-1-"));
        CHECK_EQ(field(file, labels + 7 * 16, 16), std::string("EEG 8+-8-"));
        CHECK_EQ(field(file, labels + 8 * 16, 16), std::string("BDF Annotations"));
        // The annotation label is 16 octets INCLUDING its trailing space.
        CHECK_EQ(static_cast<int>(file[labels + 8 * 16 + 15]), static_cast<int>(' '));
        CHECK_EQ(field(file, dimensions, 8), std::string("uV"));
        CHECK_EQ(field(file, digMin, 8), std::string("-8388608"));
        CHECK_EQ(field(file, digMax, 8), std::string("8388607"));
        CHECK_EQ(field(file, digMin + 8 * 8, 8), std::string("-8388608"));
        CHECK_EQ(field(file, digMax + 8 * 8, 8), std::string("8388607"));
        CHECK_EQ(field(file, physMin + 8 * 8, 8), std::string("-1"));
        CHECK_EQ(field(file, physMax + 8 * 8, 8), std::string("1"));
        CHECK_EQ(field(file, prefilter, 80), std::string("HP:DC LP:65Hz"));
        CHECK_EQ(field(file, perRecord, 8), std::string("14"));
        CHECK_EQ(field(file, perRecord + 8 * 8, 8), std::string("20"));  // 60 bytes / 3

        // The scale a reader reconstructs from the 8-character fields,
        // against the converter's true step. The true maximum, 187499.9776 uV,
        // has to be written "187500": the step comes out 1/(2^24 - 1) too large, and
        // that is one step by full scale - measured by the independent reader
        // (bdf_oracle.py), which is how this check learned its bound. A
        // tolerance of 1e-6 here would have admitted sixteen.
        const double pmin = std::atof(field(file, physMin, 8).c_str());
        const double pmax = std::atof(field(file, physMax, 8).c_str());
        const double slope = (pmax - pmin) / (8388607.0 + 8388608.0);
        const double lsb = 375000.0 / 16777216.0;
        CHECK_NEAR(slope / lsb, 1.0, (1.0 + 1e-9) / 16777215.0);
        const double top = pmin + (8388607.0 + 8388608.0) * slope;
        CHECK_NEAR(top, 8388607.0 * lsb, lsb);

        medtest::section("BiosignalWriter - registros de dados");

        // Every code comes back exactly, laid out per signal.
        bool exact = true;
        const std::vector<std::int32_t>* sent[3] = {&first, &second, &first};
        for (std::size_t r = 0; r < 3; ++r) {
            const std::size_t base = headerBytes + r * recordBytes;
            for (std::size_t s = 0; s < 8; ++s) {
                for (std::size_t i = 0; i < 14; ++i) {
                    const std::int32_t got = sample24(file, base + (s * 14 + i) * 3);
                    if (got != (*sent[r])[i * 8 + s]) {
                        exact = false;
                    }
                }
            }
        }
        CHECK_MSG(exact, "um código lido de volta difere do escrito");

        // The annotation signal: the time-keeping TAL, then the events.
        const std::size_t tals = headerBytes + 8 * 14 * 3;
        const std::string first_tals(file.begin() + static_cast<std::ptrdiff_t>(tals),
                                     file.begin() + static_cast<std::ptrdiff_t>(tals + 60));
        const std::string expected = std::string("+0.356\x14\x14\0", 9) +
                                     std::string("+0.356\x14montage bipolar\x14\0", 24) +
                                     std::string("+0.4\x15" "0.056\x14lead-off: 3+\x14\0", 25);
        CHECK_EQ(first_tals.substr(0, expected.size()), expected);
        CHECK_EQ(first_tals.find_first_not_of('\0', expected.size()), std::string::npos);

        const std::size_t tals2 = tals + recordBytes;
        CHECK_EQ(std::string(file.begin() + static_cast<std::ptrdiff_t>(tals2),
                             file.begin() + static_cast<std::ptrdiff_t>(tals2 + 9)),
                 std::string("+0.412\x14\x14\0", 9));

        // The sizes a caller plans with are the bytes the writer writes.
        CHECK_EQ(BiosignalWriter::timekeepingBytes(0.356), static_cast<std::size_t>(9));
        CHECK_EQ(BiosignalWriter::annotationBytes({0.4, 0.056, "lead-off: 3+"}),
                 static_cast<std::size_t>(25));
        CHECK_EQ(BiosignalWriter::annotationBytes({0.356, -1.0, "montage bipolar"}),
                 static_cast<std::size_t>(24));

        medtest::section("BiosignalWriter - selo");

        const std::string sidecar = storage.readString("s/record.bdf.sha256").value();
        CHECK_EQ(sidecar, writer.digest() + "  record.bdf\n");
        CHECK_EQ(writer.digest(), externalSha256(dir.file("s/record.bdf")));
        keepSample(dir.file("s/record.bdf"), "continuous.bdf");
    }

    medtest::section("BiosignalWriter - descontínuo");

    {
        BiosignalHeader header = eightChannels(3);
        header.continuous = false;
        std::unique_ptr<BiosignalWriter> writer =
            BiosignalWriter::create(storage, "d.bdf", header).take();
        CHECK(writer != nullptr);
        if (writer) {
            const std::vector<std::int32_t> samples = ramp(5);
            CHECK_STATUS(writer->appendRecord(0.356, samples.data()), Status::Ok);
            // A gap of two records: allowed, and visible in the onset.
            CHECK_STATUS(writer->appendRecord(0.524, samples.data(), {{0.412, 0.112, "gap: 2 frames"}}),
                         Status::Ok);
            // Overlapping the previous record: refused.
            CHECK_STATUS(writer->appendRecord(0.55, samples.data()), Status::InvalidArgument);
            CHECK_STATUS(writer->appendRecord(0.58, samples.data()), Status::Ok);
            CHECK_STATUS(writer->finish(), Status::Ok);
            keepSample(dir.file("d.bdf"), "discontinuous.bdf");
            const std::vector<unsigned char> file = slurp(dir.file("d.bdf"));
            CHECK_EQ(field(file, 192, 44), std::string("BDF+D"));
            const std::size_t tals = headerBytes + recordBytes + 8 * 14 * 3;
            CHECK_EQ(std::string(file.begin() + static_cast<std::ptrdiff_t>(tals),
                                 file.begin() + static_cast<std::ptrdiff_t>(tals + 9)),
                     std::string("+0.524\x14\x14\0", 9));
        }
    }

    medtest::section("BiosignalWriter - recusas");

    {
        // A continuous file's onsets are its clock: one off by a millisecond
        // is refused, not written.
        std::unique_ptr<BiosignalWriter> writer =
            BiosignalWriter::create(storage, "c.bdf", eightChannels(2)).take();
        const std::vector<std::int32_t> samples = ramp(5);
        CHECK_STATUS(writer->appendRecord(0.357, samples.data()), Status::InvalidArgument);
        CHECK_STATUS(writer->appendRecord(0.356, samples.data()), Status::Ok);

        // A code the format cannot hold is refused, never clipped.
        std::vector<std::int32_t> wide = samples;
        wide[17] = 8388608;
        CHECK_STATUS(writer->appendRecord(0.412, wide.data()), Status::OutOfRange);

        // Fewer records than the header promised: the header would be false.
        CHECK_STATUS(writer->finish(), Status::IntegrityError);
        CHECK(!storage.exists("c.bdf.sha256"));

        // An annotation that cannot fit any record, refused at once.
        const std::string longText(80, 'x');
        CHECK_STATUS(writer->appendRecord(0.412, samples.data(), {{0.4, -1.0, longText}}),
                     Status::InvalidArgument);
        // A control character in an annotation would end the TAL early.
        CHECK_STATUS(writer->appendRecord(0.412, samples.data(), {{0.4, -1.0, "a\x14" "b"}}),
                     Status::InvalidArgument);

        // Three annotations that each fit, but not together: the ones that
        // do not fit wait for the next record - and when there is none,
        // finish() says so instead of dropping them.
        CHECK_STATUS(writer->appendRecord(0.412, samples.data(),
                                          {{0.41, -1.0, "evento um, de texto longo"},
                                           {0.42, -1.0, "evento dois, de texto longo"},
                                           {0.43, -1.0, "evento três"}}),
                     Status::Ok);
        CHECK_STATUS(writer->finish(), Status::OutOfRange);
    }

    {
        // Carried annotations are written, in order, in the next record.
        std::unique_ptr<BiosignalWriter> writer =
            BiosignalWriter::create(storage, "k.bdf", eightChannels(2)).take();
        const std::vector<std::int32_t> samples = ramp(5);
        CHECK_STATUS(writer->appendRecord(0.356, samples.data(),
                                          {{0.36, -1.0, "evento um, de texto longo"},
                                           {0.37, -1.0, "evento dois"}}),
                     Status::Ok);
        CHECK_STATUS(writer->appendRecord(0.412, samples.data()), Status::Ok);
        CHECK_STATUS(writer->finish(), Status::Ok);
        const std::vector<unsigned char> file = slurp(dir.file("k.bdf"));
        const std::size_t tals2 = headerBytes + recordBytes + 8 * 14 * 3;
        const std::string second(file.begin() + static_cast<std::ptrdiff_t>(tals2),
                                 file.begin() + static_cast<std::ptrdiff_t>(tals2 + 60));
        CHECK_EQ(second.substr(0, 9 + 18),
                 std::string("+0.412\x14\x14\0", 9) + std::string("+0.37\x14" "evento dois\x14\0", 18));
    }

    {
        auto refused = [&](BiosignalHeader header, const char* why) {
            Result<std::unique_ptr<BiosignalWriter>> made =
                BiosignalWriter::create(storage, "r.bdf", header);
            CHECK_MSG(!made.isOk() && made.status() == Status::InvalidArgument, why);
        };
        BiosignalHeader h = eightChannels(1);
        h.signals[0].label = "EEG 17-characters";
        refused(h, "rótulo de 17 caracteres aceito");
        h = eightChannels(1);
        h.signals[0].label = "BDF Annotations";
        refused(h, "rótulo do sinal de anotações aceito para um sinal comum");
        h = eightChannels(1);
        h.year = 1984;
        refused(h, "ano fora de 1985..2084 aceito");
        // 1/3 s fits as "0.333333", 0.3 us short: harmless once, and a third
        // of a millisecond over a thousand records - which is what a reader
        // placing record k at k * duration would get.
        h = eightChannels(1000);
        h.recordDurationSeconds = 1.0 / 3.0;
        refused(h, "duração cuja deriva no arquivo passa de 1 us aceita");
        h = eightChannels(1);
        h.recordDurationSeconds = 1.0 / 3.0;
        CHECK_MSG(BiosignalWriter::create(storage, "r.bdf", h).isOk(),
                  "a mesma duração, num registro só, recusada sem deriva nenhuma");
        h = eightChannels(1);
        h.annotationBytesPerRecord = 6;
        refused(h, "espaço de anotação menor que o próprio início do registro aceito");
        h = eightChannels(1);
        h.signals[2].physicalMaximum = h.signals[2].physicalMinimum;
        refused(h, "mínimo físico igual ao máximo aceito");
        h = eightChannels(1);
        h.signals[2].digitalMaximum = 8388608;
        refused(h, "faixa digital além de 24 bits aceita");
        h = eightChannels(1);
        h.patient = "Jos\xc3\xa9";
        refused(h, "campo de paciente fora de ASCII aceito");
    }

    {
        // create() starts from nothing: a stale file and its sidecar go.
        CHECK(storage.exists("d.bdf.sha256"));
        std::unique_ptr<BiosignalWriter> writer =
            BiosignalWriter::create(storage, "d.bdf", eightChannels(1)).take();
        CHECK(writer != nullptr);
        CHECK(!storage.exists("d.bdf.sha256"));
    }
}
