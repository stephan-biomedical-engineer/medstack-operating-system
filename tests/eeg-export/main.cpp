// SPDX-License-Identifier: MIT
//
// eeg-record-export's core against synthetic sessions, written exactly as the
// acquisition service writes them: metadata.json, and raw.bin made of frames
// from the framework's own encoder. Then every way a record can arrive
// damaged (implementation_plan_bdf.md, phase 3): a scale that is not the one
// the samples were taken at, a corrupted frame, a truncated tail, a session
// from before the record declared its scale, a sequence that skips.
//
// The files the converter writes are read back here byte by byte. That is the
// same reading of the format the writer was built from; the reading that is
// not is scripts/amp-bench.py's "bdf" subcommand, run on the sessions this
// suite leaves in MED_BDF_SAMPLE_DIR.

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalStorage.h>

#include "../../meta-custom/meta-med-app/recipes-apps/eeg-acquisition-service/files/src/record_export.h"
#include "check.h"
#include "temp_dir.h"

using namespace med;
using medtest::TempDir;

namespace {

constexpr int kChannels = 8;
constexpr int kPerFrame = 14;
constexpr double kFullScaleUv = 187500.0;  // 4.5 V / gain 24
const double kLsbNv = kFullScaleUv * 1000.0 / 8388608.0;

/// A code for every sample that reaches both 24-bit extremes and both signs.
std::int32_t codeOf(std::uint64_t sequence, int i) {
    if (i == 0) {
        return (sequence % 2 == 0) ? 8388607 : -8388608;
    }
    const long long mixed = static_cast<long long>((sequence * 131 + static_cast<std::uint64_t>(i) * 7919) % 16777216ULL);
    return static_cast<std::int32_t>(mixed - 8388608);
}

/// The firmware's conversion (afe_code_to_nv): code times step, to the nanovolt.
double nanovoltsOf(std::int32_t code) {
    const double nv = static_cast<double>(code) * kLsbNv;
    return std::floor(nv + 0.5);
}

std::string metadata(bool withScale, const std::string& extra = "") {
    std::string m = "{\n"
                    "  \"session\": \"2026-10-09T02:33:37.356Z\",\n"
                    "  \"device_id\": \"eeg0\",\n"
                    "  \"driver\": \"rpmsg\",\n"
                    "  \"model\": \"MedPlatform AMP front-end (rpmsg)\",\n"
                    "  \"link\": \"amp\",\n"
                    "  \"channels\": 8,\n"
                    "  \"sample_rate_hz\": 250.000000,\n"
                    "  \"unit\": \"uV\",\n"
                    "  \"config_digest\": \"079e01425cc7c647eae4accbe8e1f350e9cdce8cab15f19ab6b3e5019045558f\",\n"
                    "  \"software_version\": \"1.0.0\",\n";
    if (withScale) {
        m += "  \"full_scale_uv\": 187500,\n  \"resolution_bits\": 24,\n";
    }
    m += "  \"montage\": \"bipolar\",\n"
         "  \"prefiltering\": \"HP:DC LP:65Hz\",\n"
         "  \"electrodes\": null,\n"
         "  \"reference\": null,\n"
         "  \"prescription\": {\n"
         "    \"afe.gain\": 24,\n"
         "    \"afe.test_signal\": \"off\"\n"
         "  },\n" +
         extra +
         "  \"clock_synchronized\": false,\n"
         "  \"utc_offset\": \"-03:00\"\n"
         "}\n";
    return m;
}

std::vector<unsigned char> frameBytes(std::uint64_t sequence, std::uint16_t detached = 0) {
    SampleFrame frame;
    frame.sequence = sequence;
    frame.channelCount = kChannels;
    frame.samplesPerChannel = kPerFrame;
    frame.samples.resize(kChannels * kPerFrame);
    for (int i = 0; i < kChannels * kPerFrame; ++i) {
        frame.samples[static_cast<std::size_t>(i)] = nanovoltsOf(codeOf(sequence, i)) / 1000.0;
    }
    frame.leadOff.monitoredPositive = 0x00FF;
    frame.leadOff.monitoredNegative = 0x00FF;
    frame.leadOff.offPositive = detached;
    std::vector<unsigned char> out;
    amp::encodeFrame(frame, 250.0, out);
    return out;
}

std::vector<unsigned char> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>());
}

/// A header field, or "<fora do arquivo>": a file that is missing or short
/// must fail the check that reads it, not crash the suite - an injection that
/// aborts the runner hides which check would have seen it.
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

/// The codes of channel c, record r, read back from a BDF+ file.
std::vector<std::int32_t> codesAt(const std::vector<unsigned char>& file, std::size_t r, int c) {
    std::vector<std::int32_t> out;
    const std::size_t ns = static_cast<std::size_t>(std::atoi(field(file, 252, 4).c_str()));
    const std::size_t headerBytes = static_cast<std::size_t>(std::atoi(field(file, 184, 8).c_str()));
    if (ns == 0) {
        return out;
    }
    const std::size_t samplesField = 256 + ns * (16 + 80 + 8 + 8 + 8 + 8 + 8 + 80);
    const std::size_t annotationSamples =
        static_cast<std::size_t>(std::atoi(field(file, samplesField + (ns - 1) * 8, 8).c_str()));
    const std::size_t recordBytes =
        static_cast<std::size_t>(kChannels * kPerFrame * 3) + annotationSamples * 3;
    const std::size_t base =
        headerBytes + r * recordBytes + static_cast<std::size_t>(c * kPerFrame * 3);
    if (headerBytes == 0 || base + static_cast<std::size_t>(kPerFrame * 3) > file.size()) {
        return out;
    }
    for (int i = 0; i < kPerFrame; ++i) {
        const std::size_t o = base + static_cast<std::size_t>(i * 3);
        std::uint32_t raw = static_cast<std::uint32_t>(file[o]) |
                            (static_cast<std::uint32_t>(file[o + 1]) << 8) |
                            (static_cast<std::uint32_t>(file[o + 2]) << 16);
        if ((raw & 0x800000u) != 0) {
            raw |= 0xFF000000u;
        }
        out.push_back(static_cast<std::int32_t>(raw));
    }
    return out;
}

bool contains(const std::vector<unsigned char>& file, const std::string& text) {
    return std::search(file.begin(), file.end(), text.begin(), text.end()) != file.end();
}

/// A session directory as the service writes it, with frames `sequences`.
void writeSession(MedicalStorage& storage, const std::string& dir, const std::string& meta,
                  const std::vector<std::uint64_t>& sequences) {
    storage.writeString(dir + "/metadata.json", meta);
    for (const std::uint64_t sequence : sequences) {
        const std::uint16_t detached = (sequence >= 3 && sequence < 5) ? 0x0004 : 0;
        const std::vector<unsigned char> bytes = frameBytes(sequence, detached);
        storage.append(dir + "/raw.bin", bytes.data(), bytes.size());
    }
}

/// Every channel of every record of `file` against the frames it came from.
bool codesMatch(const std::vector<unsigned char>& file, const std::vector<std::uint64_t>& sequences) {
    for (std::size_t r = 0; r < sequences.size(); ++r) {
        for (int c = 0; c < kChannels; ++c) {
            const std::vector<std::int32_t> got = codesAt(file, r, c);
            if (got.size() != static_cast<std::size_t>(kPerFrame)) {
                return false;
            }
            for (int i = 0; i < kPerFrame; ++i) {
                if (got[static_cast<std::size_t>(i)] != codeOf(sequences[r], i * kChannels + c)) {
                    return false;
                }
            }
        }
    }
    return true;
}

void keep(const TempDir& dir, const std::string& session, const std::vector<std::string>& files) {
    const char* target = std::getenv("MED_BDF_SAMPLE_DIR");
    if (target == nullptr) {
        return;
    }
    const std::string out = std::string(target) + "/" + session;
    ::mkdir(out.c_str(), 0755);
    for (const std::string& name : files) {
        std::ifstream in(dir.file("eeg/" + session + "/" + name), std::ios::binary);
        std::ofstream copy(out + "/" + name, std::ios::binary);
        copy << in.rdbuf();
    }
}

std::vector<std::uint64_t> range(std::uint64_t from, std::uint64_t to) {
    std::vector<std::uint64_t> out;
    for (std::uint64_t s = from; s < to; ++s) {
        out.push_back(s);
    }
    return out;
}

}  // namespace

int main() {
    TempDir dir;
    StorageConfig config;
    config.root = dir.path();
    config.nameSpace = "eeg";
    config.requireEncryptedBacking = false;
    std::unique_ptr<MedicalStorage> storage = MedicalStorage::open(config).take();
    CHECK(storage != nullptr);
    if (!storage) {
        return medtest::report();
    }

    medtest::section("metadata.json");
    {
        Result<eeg::SessionMetadata> m = eeg::parseMetadata(metadata(true));
        CHECK_MSG(m.isOk(), m.message());
        CHECK_EQ(static_cast<int>(m.value().channels), 8);
        CHECK_EQ(m.value().fullScaleUv, 187500.0);
        CHECK_EQ(m.value().testSignal, std::string("off"));
        CHECK(m.value().electrodes.empty());
        CHECK(!eeg::parseMetadata("{\"session\": ").isOk());
        CHECK(!eeg::parseMetadata(metadata(true) + "x").isOk());
        // A session from before the record declared its scale (D7).
        Result<eeg::SessionMetadata> old = eeg::parseMetadata(metadata(false));
        CHECK(!old.isOk());
        CHECK_MSG(old.message().find("full_scale_uv") != std::string::npos, old.message());
    }

    medtest::section("uma sessão íntegra");
    const std::vector<std::uint64_t> clean = range(100, 160);
    {
        writeSession(*storage, "session-a", metadata(true), clean);
        storage->writeString("session-a/closed.json", "{}");
        Result<eeg::ExportSummary> done = eeg::exportSession(*storage, "session-a", false);
        CHECK_MSG(done.isOk(), done.message());
        if (done) {
            const eeg::ExportSummary& s = done.value();
            CHECK_EQ(s.files.size(), static_cast<std::size_t>(1));
            CHECK(!s.files.empty() && s.files[0].name == "record.bdf");
            CHECK_EQ(static_cast<long long>(s.frames), 60LL);
            CHECK_EQ(static_cast<long long>(s.missing + s.failedCrc + s.partialBytes), 0LL);
            CHECK(storage->exists("session-a/record.bdf.sha256"));

            const std::vector<unsigned char> f = slurp(dir.file("eeg/session-a/record.bdf"));
            CHECK_EQ(field(f, 192, 44), std::string("BDF+C"));
            CHECK_EQ(field(f, 236, 8), std::string("60"));
            CHECK_EQ(field(f, 244, 8), std::string("0.056"));
            // Local time: 02:33:37.356 UTC at -03:00 is 23:33:37 the day before.
            CHECK_EQ(field(f, 168, 8), std::string("08.10.26"));
            CHECK_EQ(field(f, 176, 8), std::string("23.33.37"));
            CHECK(contains(f, "+0.356\x14\x14"));
            CHECK_EQ(field(f, 256, 16), std::string("EEG 1+-1-"));
            CHECK_MSG(codesMatch(f, clean), "um código do BDF difere do quadro de origem");
            CHECK(contains(f, "link amp"));
            CHECK(contains(f, "montage bipolar"));
            CHECK(contains(f, "clock not synchronized"));
            CHECK(contains(f, "UTC offset -03:00"));
            CHECK(contains(f, "electrode contact: all attached"));
            CHECK(!contains(f, "record interrupted"));

            // Deterministic: the same record gives the same file (D1).
            Result<eeg::ExportSummary> again = eeg::exportSession(*storage, "session-a", false);
            CHECK(again.isOk() && !again.value().files.empty() && !s.files.empty() &&
                  again.value().files[0].sha256 == s.files[0].sha256);
        }
        keep(dir, "session-a", {"raw.bin", "metadata.json", "record.bdf"});
    }

    medtest::section("contato de eletrodo, nas mudanças");
    {
        // Frames 3 and 4 of a session from 0 have channel 3's positive input
        // off: one annotation when it goes, one when it comes back.
        writeSession(*storage, "session-l", metadata(true), range(0, 10));
        Result<eeg::ExportSummary> done = eeg::exportSession(*storage, "session-l", false);
        CHECK(done.isOk());
        const std::vector<unsigned char> f = slurp(dir.file("eeg/session-l/record.bdf"));
        CHECK(contains(f, "electrode contact: detached 3+"));
        CHECK(contains(f, std::string("+0.524\x14" "electrode contact: detached 3+", 37)));
        CHECK(contains(f, std::string("+0.636\x14" "electrode contact: all attached", 37)));
    }

    medtest::section("montagem referencial");
    {
        // What the producer sends in a referential montage: every positive
        // input monitored, and ONE negative bit, which is the reference
        // electrode and not "the negative input of channel 1".
        auto replace = [](std::string text, const std::string& from, const std::string& to) {
            const std::size_t at = text.find(from);
            if (at != std::string::npos) {
                text.replace(at, from.size(), to);
            }
            return text;
        };
        auto referentialFrame = [](std::uint64_t sequence, std::uint16_t offPositive,
                                   std::uint16_t offNegative) {
            SampleFrame frame;
            frame.sequence = sequence;
            frame.channelCount = kChannels;
            frame.samplesPerChannel = kPerFrame;
            frame.samples.resize(kChannels * kPerFrame);
            for (int i = 0; i < kChannels * kPerFrame; ++i) {
                frame.samples[static_cast<std::size_t>(i)] = nanovoltsOf(codeOf(sequence, i)) / 1000.0;
            }
            frame.leadOff.monitoredPositive = 0x00FF;
            frame.leadOff.monitoredNegative = 0x0001;
            frame.leadOff.offPositive = offPositive;
            frame.leadOff.offNegative = offNegative;
            std::vector<unsigned char> out;
            amp::encodeFrame(frame, 250.0, out);
            return out;
        };
        auto write = [&](const std::string& session, const std::string& meta) {
            storage->writeString(session + "/metadata.json", meta);
            for (std::uint64_t q = 0; q < 10; ++q) {
                // Frames 2-3: the reference comes off. Frames 6-7: channel 2's
                // own electrode, with the reference attached.
                const std::vector<unsigned char> bytes = referentialFrame(
                    q, (q == 6 || q == 7) ? 0x0002 : 0, (q == 2 || q == 3) ? 0x0001 : 0);
                storage->append(session + "/raw.bin", bytes.data(), bytes.size());
            }
        };

        const std::string named = replace(
            replace(replace(metadata(true), "\"montage\": \"bipolar\"", "\"montage\": \"referential\""),
                    "\"electrodes\": null",
                    "\"electrodes\": [\"Fp1\", \"Fp2\", \"C3\", \"C4\", \"P3\", \"P4\", \"O1\", \"O2\"]"),
            "\"reference\": null", "\"reference\": \"A1A2\"");
        write("session-ref", named);
        Result<eeg::ExportSummary> done = eeg::exportSession(*storage, "session-ref", false);
        CHECK_MSG(done.isOk(), done.message());
        const std::vector<unsigned char> f = slurp(dir.file("eeg/session-ref/record.bdf"));
        CHECK_EQ(field(f, 256, 16), std::string("EEG Fp1-A1A2"));
        CHECK_EQ(field(f, 256 + 7 * 16, 16), std::string("EEG O2-A1A2"));
        CHECK(contains(f, "montage referential, reference A1A2"));
        CHECK(!contains(f, "montage bipolar"));
        CHECK(contains(f, "electrode contact: detached A1A2\x14"));
        CHECK(contains(f, "electrode contact: detached Fp2\x14"));
        // The reference is never reported as an input of channel 1.
        CHECK(!contains(f, "detached 1-") && !contains(f, "detached Fp1"));
        CHECK_MSG(codesMatch(f, range(0, 10)), "um código do BDF referencial difere do quadro");

        // Nothing declared: generic and still referential (D10).
        const std::string generic =
            replace(metadata(true), "\"montage\": \"bipolar\"", "\"montage\": \"referential\"");
        write("session-refgen", generic);
        done = eeg::exportSession(*storage, "session-refgen", false);
        CHECK_MSG(done.isOk(), done.message());
        const std::vector<unsigned char> g = slurp(dir.file("eeg/session-refgen/record.bdf"));
        CHECK_EQ(field(g, 256, 16), std::string("EEG 1-REF"));
        CHECK(contains(g, "montage referential, reference REF"));
        CHECK(contains(g, "electrode contact: detached REF\x14"));
        CHECK(contains(g, "electrode contact: detached 2+\x14"));
        CHECK(!contains(g, "detached 1-"));
    }

    medtest::section("injeção: escala diferente da declarada (D7)");
    {
        // The samples were taken at gain 24; the metadata says gain 12.
        std::string wrong = metadata(true);
        wrong.replace(wrong.find("187500,"), 7, "375000,");
        writeSession(*storage, "session-s", wrong, range(0, 5));
        Result<eeg::ExportSummary> done = eeg::exportSession(*storage, "session-s", false);
        CHECK_STATUS(done.status(), Status::IntegrityError);
        CHECK_MSG(done.message().find("frame 0") != std::string::npos, done.message());
        CHECK(!storage->exists("session-s/record.bdf.sha256"));
    }

    medtest::section("injeção: um byte trocado num quadro");
    {
        writeSession(*storage, "session-c", metadata(true), range(0, 20));
        Result<std::vector<std::uint8_t>> raw = storage->read("session-c/raw.bin");
        std::vector<std::uint8_t> bytes = raw.value();
        bytes[7 * 496 + 200] ^= 0x10;  // a sample of frame 7
        storage->write("session-c/raw.bin", bytes.data(), bytes.size());

        Result<eeg::ExportSummary> done = eeg::exportSession(*storage, "session-c", false);
        CHECK_MSG(done.isOk(), done.message());
        if (done) {
            const eeg::ExportSummary& s = done.value();
            CHECK_EQ(static_cast<long long>(s.failedCrc), 1LL);
            CHECK_EQ(static_cast<long long>(s.missing), 1LL);
            CHECK_EQ(s.files.size(), static_cast<std::size_t>(2));
            if (s.files.size() == 2) {
                CHECK_EQ(s.files[0].name, std::string("record-001.bdf"));
                CHECK_EQ(static_cast<long long>(s.files[0].records), 7LL);
                CHECK_EQ(static_cast<long long>(s.files[1].records), 12LL);
            }
            const std::vector<unsigned char> a = slurp(dir.file("eeg/session-c/record-001.bdf"));
            const std::vector<unsigned char> b = slurp(dir.file("eeg/session-c/record-002.bdf"));
            CHECK(codesMatch(a, range(0, 7)));
            CHECK(codesMatch(b, range(8, 20)));
            CHECK(contains(a, "gap follows: 1 frames (0.056 s)"));
            CHECK(contains(b, "after gap: 1 frames (0.056 s), 1 failed CRC"));
            // The second stretch starts 8 frames after the first: 0.448 s
            // later, so at 23:33:37.804 local.
            CHECK_EQ(field(b, 176, 8), std::string("23.33.37"));
            CHECK(contains(b, "+0.804\x14\x14"));
            CHECK(!storage->exists("session-c/record.bdf"));
        }
        keep(dir, "session-c", {"raw.bin", "metadata.json", "record-001.bdf", "record-002.bdf"});
    }

    medtest::section("injeção: raw.bin truncado no meio de um quadro");
    {
        writeSession(*storage, "session-t", metadata(true), range(0, 6));
        Result<std::vector<std::uint8_t>> raw = storage->read("session-t/raw.bin");
        std::vector<std::uint8_t> bytes = raw.value();
        bytes.resize(bytes.size() - 300);
        storage->write("session-t/raw.bin", bytes.data(), bytes.size());

        Result<eeg::ExportSummary> done = eeg::exportSession(*storage, "session-t", true);
        CHECK_MSG(done.isOk(), done.message());
        if (done) {
            CHECK_EQ(static_cast<long long>(done.value().frames), 5LL);
            CHECK_EQ(static_cast<long long>(done.value().partialBytes), 196LL);
            const std::vector<unsigned char> f = slurp(dir.file("eeg/session-t/record.bdf"));
            CHECK(contains(f, "partial frame discarded (196 bytes)"));
            CHECK(contains(f, "record interrupted"));
            CHECK(codesMatch(f, range(0, 5)));
        }
    }

    medtest::section("injeção: sessão sem escala no metadado");
    {
        writeSession(*storage, "session-o", metadata(false), range(0, 3));
        Result<eeg::ExportSummary> done = eeg::exportSession(*storage, "session-o", false);
        CHECK_STATUS(done.status(), Status::InvalidArgument);
        CHECK(!storage->exists("session-o/record.bdf"));
    }

    medtest::section("lacuna de sequência e reinício do produtor");
    {
        std::vector<std::uint64_t> skips = range(0, 5);
        for (std::uint64_t s = 9; s < 12; ++s) skips.push_back(s);   // 4 missing
        for (std::uint64_t s = 0; s < 3; ++s) skips.push_back(s);    // restarted
        writeSession(*storage, "session-g", metadata(true), skips);
        Result<eeg::ExportSummary> done = eeg::exportSession(*storage, "session-g", false);
        CHECK_MSG(done.isOk(), done.message());
        if (done) {
            CHECK_EQ(done.value().files.size(), static_cast<std::size_t>(3));
            CHECK_EQ(static_cast<long long>(done.value().missing), 4LL);
            CHECK_EQ(static_cast<long long>(done.value().restarts), 1LL);
            const std::vector<unsigned char> c = slurp(dir.file("eeg/session-g/record-003.bdf"));
            CHECK(contains(c, "after sequence restart, gap unknown"));
        }
    }

    medtest::section("nada a converter");
    {
        storage->writeString("session-e/metadata.json", metadata(true));
        const unsigned char junk[100] = {1, 2, 3};
        storage->write("session-e/raw.bin", junk, sizeof(junk));
        CHECK_STATUS(eeg::exportSession(*storage, "session-e", false).status(),
                     Status::InvalidArgument);
    }

    medtest::section("marcadores");
    {
        const std::string refused = eeg::refusalMarker("say \"no\"\nnow");
        CHECK(refused.find("\\\"no\\\" now") != std::string::npos);
    }

    return medtest::report();
}
