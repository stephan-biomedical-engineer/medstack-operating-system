// SPDX-License-Identifier: MIT
//
// MedicalConfiguration: the store that decides how the device behaves
// clinically, and the two things that separate it from "read an ini file" -
// the integrity sidecar and the declared safety limits.

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "MedicalConfiguration.h"
#include "check.h"
#include "temp_dir.h"

using namespace med;
using medtest::TempDir;

namespace {

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out << content;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

void testConfiguration() {
    medtest::section("MedicalConfiguration - leitura e tipos");

    {
        std::unique_ptr<MedicalConfiguration> config = MedicalConfiguration::empty();
        CHECK(config != nullptr);

        CHECK_STATUS(config->set("device.driver", "simulated"), Status::Ok);
        CHECK_STATUS(config->setDouble("afe.gain", 24.0), Status::Ok);
        CHECK_STATUS(config->setInt("acquisition.channels", 8), Status::Ok);
        CHECK_STATUS(config->set("storage.require_encryption", "true"), Status::Ok);

        CHECK(config->has("device.driver"));
        CHECK(!config->has("device.nonexistent"));
        CHECK(config->dirty());

        CHECK_EQ(config->getString("device.driver", "?"), std::string("simulated"));
        CHECK_EQ(config->getInt("acquisition.channels", -1), 8LL);
        CHECK_NEAR(config->getDouble("afe.gain", -1.0), 24.0, 1e-9);
        CHECK(config->getBool("storage.require_encryption", false));

        // A missing key is NotFound, not a zero. A calibration coefficient that
        // silently defaults to 0 scales every measurement by nothing.
        Result<double> missing = config->getDouble("afe.absent");
        CHECK(!missing.isOk());
        CHECK_STATUS(missing.status(), Status::NotFound);

        // The explicit-fallback overloads exist for the cases where a default
        // is genuinely correct, and they are a different call on purpose.
        CHECK_NEAR(config->getDouble("afe.absent", 3.5), 3.5, 1e-9);

        const std::vector<std::string> keys = config->keys();
        CHECK_EQ(static_cast<long long>(keys.size()), 4LL);
    }

    medtest::section("MedicalConfiguration - o comentário inline continua sendo valor");

    {
        // This locks in a decision, not an accident. An inline comment after a
        // value becomes part of the value, because a format where '#' inside a
        // value is special breaks every value that legitimately contains '#'.
        //
        // It cost an image build and a QEMU boot to find once (six assertions
        // failed at the same time, and the cause was `afe.gain = 24  # PGA`),
        // and the fix was deliberately NOT to change this parser: it was to
        // make do_seal_configuration refuse the file at build time. If someone
        // ever "fixes" the parser instead, this check fails and points at the
        // reason. See implementation_plan_iio_afe.md §13.5.
        TempDir dir;
        CHECK(dir.valid());
        const std::string path = dir.file("inline.conf");
        writeFile(path, "afe.gain = 24   # PGA\n");

        Result<std::unique_ptr<MedicalConfiguration>> loaded =
            MedicalConfiguration::load(path, false);
        CHECK(loaded.isOk());
        if (loaded.isOk()) {
            const std::unique_ptr<MedicalConfiguration>& config = loaded.value();
            CHECK_EQ(config->getString("afe.gain", "?"), std::string("24   # PGA"));
            // And therefore it is not a number, which is what stops the device.
            Result<double> asNumber = config->getDouble("afe.gain");
            CHECK(!asNumber.isOk());
        }
    }

    medtest::section("MedicalConfiguration - comentários e forma do arquivo");

    {
        TempDir dir;
        CHECK(dir.valid());
        const std::string path = dir.file("shape.conf");
        writeFile(path,
                  "# comentário de linha inteira\n"
                  "; outro estilo de comentário\n"
                  "\n"
                  "   device.id   =   eeg0   \n"
                  "acquisition.sample_rate_hz=250\n"
                  "linha sem separador\n"
                  "empty.value =\n");

        Result<std::unique_ptr<MedicalConfiguration>> loaded =
            MedicalConfiguration::load(path, false);
        CHECK(loaded.isOk());
        if (loaded.isOk()) {
            const std::unique_ptr<MedicalConfiguration>& config = loaded.value();
            // Surrounding whitespace is trimmed from both key and value.
            CHECK_EQ(config->getString("device.id", "?"), std::string("eeg0"));
            CHECK_EQ(config->getInt("acquisition.sample_rate_hz", -1), 250LL);
            CHECK(!config->has("linha sem separador"));
            CHECK(config->has("empty.value"));
            CHECK_EQ(config->getString("empty.value", "?"), std::string(""));
        }
    }

    medtest::section("MedicalConfiguration - selo de integridade");

    {
        TempDir dir;
        CHECK(dir.valid());
        const std::string path = dir.file("sealed.conf");

        // A store with no sidecar is IntegrityError even though the file parses
        // perfectly. An unverifiable calibration table is not a usable one, and
        // "the file was there" is not the same claim as "the file is the one
        // that was validated".
        writeFile(path, "afe.gain = 24\n");
        Result<std::unique_ptr<MedicalConfiguration>> unsealed =
            MedicalConfiguration::load(path, true);
        CHECK(!unsealed.isOk());
        CHECK_STATUS(unsealed.status(), Status::IntegrityError);

        // The same file loads when integrity is not required - the escape hatch
        // exists and is a different call, so using it is visible in the source.
        Result<std::unique_ptr<MedicalConfiguration>> unchecked =
            MedicalConfiguration::load(path, false);
        CHECK(unchecked.isOk());
    }

    {
        TempDir dir;
        CHECK(dir.valid());
        const std::string path = dir.file("committed.conf");

        std::unique_ptr<MedicalConfiguration> config = MedicalConfiguration::empty(path);
        CHECK_STATUS(config->setDouble("afe.gain", 24.0), Status::Ok);
        CHECK_STATUS(config->set("device.driver", "simulated"), Status::Ok);
        CHECK_STATUS(config->commit(), Status::Ok);
        CHECK(!config->dirty());

        // commit() writes the store and the sidecar, in that order.
        CHECK(!readFile(path).empty());
        const std::string sidecar = readFile(path + ".sha256");
        CHECK_EQ(static_cast<long long>(sidecar.size() >= 64), 1LL);

        // The digest in memory is the digest on disk.
        CHECK_EQ(sidecar.substr(0, 64), config->digest());

        // And the committed store reloads under verification.
        Result<std::unique_ptr<MedicalConfiguration>> reloaded =
            MedicalConfiguration::load(path, true);
        CHECK(reloaded.isOk());
        if (reloaded.isOk()) {
            CHECK_NEAR(reloaded.value()->getDouble("afe.gain", -1.0), 24.0, 1e-9);
        }

        // Tampering with one byte of the store is detected. This is the whole
        // point of the sidecar: an eMMC that flips a bit in a gain, or an
        // operator who edits the file outside the application, must not produce
        // a device that scales patient measurements by a number nobody
        // approved.
        std::string tampered = readFile(path);
        const std::string::size_type where = tampered.find("24");
        CHECK(where != std::string::npos);
        if (where != std::string::npos) {
            tampered[where] = '9';
            writeFile(path, tampered);
        }
        Result<std::unique_ptr<MedicalConfiguration>> corrupted =
            MedicalConfiguration::load(path, true);
        CHECK(!corrupted.isOk());
        CHECK_STATUS(corrupted.status(), Status::IntegrityError);
    }

    {
        TempDir dir;
        CHECK(dir.valid());
        const std::string path = dir.file("digest.conf");
        std::unique_ptr<MedicalConfiguration> config = MedicalConfiguration::empty(path);

        config->set("a", "1");
        const std::string before = config->digest();
        config->set("a", "2");
        const std::string after = config->digest();
        // A digest that did not move when a value moved would make the sidecar
        // decorative.
        CHECK(before != after);
        CHECK_EQ(static_cast<long long>(before.size()), 64LL);
    }

    medtest::section("MedicalConfiguration - limites de segurança declarados");

    {
        std::unique_ptr<MedicalConfiguration> config = MedicalConfiguration::empty();
        config->setInt("acquisition.sample_rate_hz", 250);
        config->setInt("acquisition.channels", 8);
        config->setDouble("afe.gain", 24.0);
        config->set("afe.reference_uv", "não é número");

        std::vector<SafetyLimit> limits;

        // A discrete set, not a range. A converter of this class offers 250,
        // 500, 1k, 2k, 4k, 8k or 16k and nothing between, so 300 is not a
        // slightly-off request - it is an unimplementable one, and letting the
        // driver round it produces a device acquiring at a rate its own session
        // metadata does not name.
        SafetyLimit rate;
        rate.key = "acquisition.sample_rate_hz";
        rate.minimum = 250.0;
        rate.maximum = 16000.0;
        rate.allowed = {250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
        rate.description = "taxa de amostragem do conversor";
        limits.push_back(rate);

        SafetyLimit channels;
        channels.key = "acquisition.channels";
        channels.minimum = 8.0;
        channels.maximum = 64.0;
        channels.multipleOf = 8.0;
        limits.push_back(channels);

        SafetyLimit gain;
        gain.key = "afe.gain";
        gain.minimum = 1.0;
        gain.maximum = 24.0;
        limits.push_back(gain);

        SafetyLimit reference;
        reference.key = "afe.reference_uv";
        reference.minimum = 1.0;
        reference.maximum = 5000000.0;
        limits.push_back(reference);

        SafetyLimit required;
        required.key = "afe.absent_but_required";
        required.minimum = 0.0;
        required.maximum = 1.0;
        required.required = true;
        required.description = "parâmetro do qual a partida segura depende";
        limits.push_back(required);

        SafetyLimit optional;
        optional.key = "afe.absent_and_optional";
        optional.minimum = 0.0;
        optional.maximum = 1.0;
        optional.required = false;
        limits.push_back(optional);

        const std::vector<SafetyViolation> violations = config->validate(limits);

        // Exactly two: the unparsable reference and the missing required key.
        // Note what is NOT reported - the optional absent key, which is the
        // difference between "not configured" and "configured wrong".
        CHECK_EQ(static_cast<long long>(violations.size()), 2LL);

        bool sawUnparsable = false;
        bool sawMissing = false;
        for (const SafetyViolation& violation : violations) {
            if (violation.key == "afe.reference_uv") {
                sawUnparsable = true;
                CHECK(violation.present);
                CHECK(violation.reason.find("not a number") != std::string::npos);
            }
            if (violation.key == "afe.absent_but_required") {
                sawMissing = true;
                // present=false is what tells an operator "nobody set this",
                // as opposed to "somebody set this wrong".
                CHECK(!violation.present);
                CHECK(violation.reason.find("partida segura") != std::string::npos);
            }
        }
        CHECK(sawUnparsable);
        CHECK(sawMissing);
    }

    {
        // Every violation is reported, not just the first: an operator needs
        // the whole picture before deciding, and a check that stops early turns
        // one fix into three round trips.
        std::unique_ptr<MedicalConfiguration> config = MedicalConfiguration::empty();
        config->setInt("acquisition.sample_rate_hz", 300);  // in range, not in the set
        config->setInt("acquisition.channels", 12);         // in range, not a multiple of 8
        config->setDouble("afe.gain", 48.0);                // out of range

        std::vector<SafetyLimit> limits;

        SafetyLimit rate;
        rate.key = "acquisition.sample_rate_hz";
        rate.minimum = 250.0;
        rate.maximum = 16000.0;
        rate.allowed = {250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
        limits.push_back(rate);

        SafetyLimit channels;
        channels.key = "acquisition.channels";
        channels.minimum = 8.0;
        channels.maximum = 64.0;
        channels.multipleOf = 8.0;
        limits.push_back(channels);

        SafetyLimit gain;
        gain.key = "afe.gain";
        gain.minimum = 1.0;
        gain.maximum = 24.0;
        limits.push_back(gain);

        const std::vector<SafetyViolation> violations = config->validate(limits);
        CHECK_EQ(static_cast<long long>(violations.size()), 3LL);
    }

    {
        // The empty vector is the only spelling of "safe to operate".
        std::unique_ptr<MedicalConfiguration> config = MedicalConfiguration::empty();
        config->setInt("acquisition.sample_rate_hz", 250);
        config->setInt("acquisition.channels", 24);
        config->setDouble("afe.gain", 24.0);

        std::vector<SafetyLimit> limits;

        SafetyLimit rate;
        rate.key = "acquisition.sample_rate_hz";
        rate.minimum = 250.0;
        rate.maximum = 16000.0;
        rate.allowed = {250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
        limits.push_back(rate);

        SafetyLimit channels;
        channels.key = "acquisition.channels";
        channels.minimum = 8.0;
        channels.maximum = 64.0;
        channels.multipleOf = 8.0;
        limits.push_back(channels);

        SafetyLimit gain;
        gain.key = "afe.gain";
        gain.minimum = 1.0;
        gain.maximum = 24.0;
        limits.push_back(gain);

        CHECK_EQ(static_cast<long long>(config->validate(limits).size()), 0LL);
    }
}
