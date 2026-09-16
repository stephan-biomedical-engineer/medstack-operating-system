// SPDX-License-Identifier: MIT
//
// MedicalDevice: the factory, each driver's option vocabulary, the simulated
// converter's arithmetic, and the wire formats of the AMP link.
//
// Options are checked per driver, because each driver owns the vocabulary of
// its transport and the framework owns none for a class of device. The first
// section is the regression test for the time it did: a front-end that was not
// a biopotential amplifier could not be configured on any driver.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "MedicalDevice.h"
#include "check.h"

using namespace med;

namespace {

DeviceConfig baseConfig(const std::string& driver) {
    DeviceConfig config;
    config.driver = driver;
    config.id = "device0";
    config.address = (driver == "iio") ? "/dev/med-afe-eeg0" : "/dev/rpmsg0";
    config.channelCount = 8;
    config.sampleRateHz = 250.0;
    config.samplesPerFrame = 14;
    return config;
}

const char* const kAllDrivers[] = {"simulated", "rpmsg", "iio"};

}  // namespace

void testDevice() {
    medtest::section("MedicalDeviceFactory - registro e recusas");

    {
        const std::vector<std::string> drivers = MedicalDeviceFactory::drivers();
        // Three built-in drivers, each named after a TRANSPORT and never after
        // a part. A driver called "ads1299" would end the portability claim
        // that implementation_plan_ads1299.md §3 checks with a grep.
        CHECK_EQ(static_cast<long long>(drivers.size()), 3LL);
        for (const char* name : kAllDrivers) {
            CHECK_MSG(std::find(drivers.begin(), drivers.end(), std::string(name)) !=
                          drivers.end(),
                      std::string("driver ausente: ") + name);
        }
        for (const std::string& name : drivers) {
            // The containment, asserted from inside the library for once
            // instead of by grepping the tree.
            std::string lowered = name;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            CHECK_MSG(lowered.find("ads") == std::string::npos &&
                          lowered.find("mcp") == std::string::npos,
                      "um driver foi batizado com um part-number: " + name);
        }
    }

    {
        // Registering an existing name fails instead of silently replacing the
        // driver: otherwise the name in a configuration file no longer
        // identifies the code that will run.
        const bool replaced = MedicalDeviceFactory::registerDriver(
            "simulated", [](const DeviceConfig&) {
                return Result<std::unique_ptr<MedicalDevice>>::fail(Status::Internal);
            });
        CHECK(!replaced);

        CHECK(!MedicalDeviceFactory::registerDriver("", nullptr));
        CHECK(!MedicalDeviceFactory::registerDriver("valid-name", nullptr));
    }

    {
        DeviceConfig config = baseConfig("nonexistent");
        Result<std::unique_ptr<MedicalDevice>> created = MedicalDeviceFactory::create(config);
        CHECK(!created.isOk());
        CHECK_STATUS(created.status(), Status::NotFound);
    }

    {
        // Geometry is checked before any driver is consulted, so the three
        // agree on what an impossible configuration is.
        DeviceConfig noChannels = baseConfig("simulated");
        noChannels.channelCount = 0;
        CHECK_STATUS(MedicalDeviceFactory::create(noChannels).status(),
                     Status::InvalidArgument);

        DeviceConfig noRate = baseConfig("simulated");
        noRate.sampleRateHz = 0.0;
        CHECK_STATUS(MedicalDeviceFactory::create(noRate).status(), Status::InvalidArgument);

        DeviceConfig noFrame = baseConfig("simulated");
        noFrame.samplesPerFrame = 0;
        CHECK_STATUS(MedicalDeviceFactory::create(noFrame).status(), Status::InvalidArgument);
    }

    medtest::section("Opções - nenhum vocabulário de classe de dispositivo no framework");

    {
        // The regression this section exists for. The framework once parsed
        // afe.gain, afe.reference_uv, afe.lead_off_detection, afe.bias_drive and
        // afe.test_signal in all three drivers, and refused anything else - so
        // a front-end that is not a biopotential amplifier could not be
        // configured on any of them. The prescription's words now mean nothing
        // here, and each driver answers them in its own transport's terms.
        //
        //   simulated - not one of its parameters: refused
        //   rpmsg     - forwarded to the producer, which is the one to judge
        //   iio       - not an attribute name (it has a '.'): refused
        const char* const prescription[] = {"afe.gain", "afe.reference_uv",
                                            "afe.lead_off_detection", "afe.bias_drive",
                                            "afe.test_signal"};
        for (const char* key : prescription) {
            DeviceConfig simulated = baseConfig("simulated");
            simulated.driverOptions[key] = "24";
            CHECK_STATUS(MedicalDeviceFactory::create(simulated).status(),
                         Status::NotSupported);

            DeviceConfig rpmsg = baseConfig("rpmsg");
            rpmsg.driverOptions[key] = "24";
            Result<std::unique_ptr<MedicalDevice>> forwarded =
                MedicalDeviceFactory::create(rpmsg);
            CHECK_MSG(forwarded.isOk(),
                      std::string("rpmsg julgou a chave ") + key +
                          " em vez de repassá-la: " + forwarded.message());

            DeviceConfig iio = baseConfig("iio");
            iio.driverOptions[key] = "24";
            CHECK_STATUS(MedicalDeviceFactory::create(iio).status(), Status::InvalidArgument);
        }
    }

    {
        // The portability claim, exercised with a device class the framework
        // was never written for: a pressure transducer. Every driver takes it
        // on its own transport's terms and none of them needs a line changed.
        DeviceConfig simulated = baseConfig("simulated");
        simulated.channelCount = 2;
        simulated.driverOptions["unit"] = "mmHg";
        simulated.driverOptions["full_scale"] = "300";
        simulated.driverOptions["resolution_bits"] = "16";
        simulated.driverOptions["tones"] = "40@1.2, 5@2.4@0.1";
        Result<std::unique_ptr<MedicalDevice>> created = MedicalDeviceFactory::create(simulated);
        CHECK_MSG(created.isOk(), "simulated recusou um transdutor de pressão: " +
                                      created.message());
        if (created.isOk()) {
            std::unique_ptr<MedicalDevice> device = created.take();
            CHECK_EQ(device->info().unit, std::string("mmHg"));
            CHECK_STATUS(device->selfTest(), Status::Ok);
            CHECK_STATUS(device->start(), Status::Ok);
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(500));
            CHECK(frame.isOk());
            if (frame.isOk()) {
                const double step = 300.0 / 32768.0;
                for (const float sample : frame.value().samples) {
                    const double codes = sample / step;
                    CHECK_MSG(std::fabs(sample) <= 300.0 &&
                                  std::fabs(codes - std::floor(codes + 0.5)) < 1e-3,
                              "amostra de pressão fora do modelo: " + std::to_string(sample));
                }
            }
            device->stop();
        }

        DeviceConfig rpmsg = baseConfig("rpmsg");
        rpmsg.driverOptions["excitation_mv"] = "5000";
        rpmsg.driverOptions["zero_offset"] = "-3";
        CHECK_MSG(MedicalDeviceFactory::create(rpmsg).isOk(),
                  "rpmsg recusou opções de outra classe de dispositivo");

        DeviceConfig iio = baseConfig("iio");
        iio.driverOptions["in_pressure_oversampling_ratio"] = "16";
        CHECK_MSG(MedicalDeviceFactory::create(iio).isOk(),
                  "iio recusou um atributo de outra classe de dispositivo");
    }

    medtest::section("Opções do driver simulado");

    {
        // With no options the simulator is neutral: no signal, no noise, no
        // quantisation, an arbitrary unit. Anything that makes it resemble a
        // particular device has to come from the application.
        DeviceConfig config = baseConfig("simulated");
        Result<std::unique_ptr<MedicalDevice>> created = MedicalDeviceFactory::create(config);
        CHECK(created.isOk());
        if (created.isOk()) {
            std::unique_ptr<MedicalDevice> device = created.take();
            CHECK_EQ(device->info().unit, std::string("a.u."));
            CHECK_STATUS(device->selfTest(), Status::Ok);
            CHECK_STATUS(device->start(), Status::Ok);
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(500));
            CHECK(frame.isOk());
            if (frame.isOk()) {
                bool allZero = true;
                for (const float sample : frame.value().samples) {
                    allZero = allZero && sample == 0.0F;
                }
                CHECK_MSG(allZero, "o simulador sem opções inventou um sinal");
            }
            device->stop();
        }
    }

    {
        struct Rejection {
            const char* key;
            const char* value;
            Status status;
            const char* why;
        };

        const Rejection rejections[] = {
            // An unknown key is an ERROR, never a silent drop.
            {"ful_scale", "300", Status::NotSupported, "typo no nome da chave"},
            {"gain", "24", Status::NotSupported, "parâmetro que o simulador não tem"},

            {"unit", "", Status::InvalidArgument, "unidade vazia"},

            {"full_scale", "0", Status::OutOfRange, "fundo de escala zero"},
            {"full_scale", "-300", Status::OutOfRange, "fundo de escala negativo"},
            {"full_scale", "abc", Status::InvalidArgument, "não numérico"},
            {"full_scale", "", Status::InvalidArgument, "vazio"},
            {"full_scale", "nan", Status::InvalidArgument, "NaN"},
            // The inline comment again, arriving here as the string it is.
            {"full_scale", "300   # mmHg", Status::InvalidArgument, "comentário inline no valor"},

            {"resolution_bits", "0", Status::OutOfRange, "resolução zero"},
            {"resolution_bits", "33", Status::OutOfRange, "resolução acima de 32"},
            {"resolution_bits", "24.5", Status::InvalidArgument, "resolução não inteira"},

            {"noise_rms", "-0.1", Status::OutOfRange, "ruído negativo"},

            {"tones", "20", Status::InvalidArgument, "tom sem frequência"},
            {"tones", "20@", Status::InvalidArgument, "frequência vazia"},
            {"tones", "a@10", Status::InvalidArgument, "amplitude não numérica"},
            {"tones", "20@-1", Status::InvalidArgument, "frequência negativa"},
            {"tones", "20@10@0.4@1", Status::InvalidArgument, "campos demais"},
            {"tones", "20@10,", Status::InvalidArgument, "entrada vazia na lista"},

            {"square", "1@2@3", Status::InvalidArgument, "onda quadrada não tem fase"},
            {"square", "1", Status::InvalidArgument, "onda quadrada sem frequência"},
        };

        for (const Rejection& rejection : rejections) {
            DeviceConfig config = baseConfig("simulated");
            config.driverOptions[rejection.key] = rejection.value;
            Result<std::unique_ptr<MedicalDevice>> created = MedicalDeviceFactory::create(config);

            const std::string context = std::string("simulated / ") + rejection.key + " = '" +
                                        rejection.value + "' (" + rejection.why + ")";
            CHECK_MSG(!created.isOk(), context + ": foi aceito");
            CHECK_MSG(created.status() == rejection.status,
                      context + ": status " + toString(created.status()) + ", esperado " +
                          toString(rejection.status));
        }

        // A step is a fraction of a range, and without a range there is none.
        DeviceConfig noRange = baseConfig("simulated");
        noRange.driverOptions["resolution_bits"] = "24";
        CHECK_STATUS(MedicalDeviceFactory::create(noRange).status(), Status::InvalidArgument);
    }

    medtest::section("Opções do driver rpmsg - o que cabe na mensagem de controle");

    {
        // Nothing is judged, but what could never be sent fails at creation,
        // identically on every target, instead of at start() against firmware.
        DeviceConfig eight = baseConfig("rpmsg");
        for (int i = 0; i < 8; ++i) {
            eight.driverOptions["option" + std::to_string(i)] = "1";
        }
        CHECK(MedicalDeviceFactory::create(eight).isOk());

        DeviceConfig nine = eight;
        nine.driverOptions["option8"] = "1";
        CHECK_STATUS(MedicalDeviceFactory::create(nine).status(), Status::InvalidArgument);

        // Key and value sizes include the terminating NUL.
        DeviceConfig longKey = baseConfig("rpmsg");
        longKey.driverOptions[std::string(amp::kControlKeySize, 'k')] = "1";
        CHECK_STATUS(MedicalDeviceFactory::create(longKey).status(), Status::InvalidArgument);

        DeviceConfig fittingKey = baseConfig("rpmsg");
        fittingKey.driverOptions[std::string(amp::kControlKeySize - 1, 'k')] =
            std::string(amp::kControlValueSize - 1, 'v');
        CHECK(MedicalDeviceFactory::create(fittingKey).isOk());

        DeviceConfig longValue = baseConfig("rpmsg");
        longValue.driverOptions["key"] = std::string(amp::kControlValueSize, 'v');
        CHECK_STATUS(MedicalDeviceFactory::create(longValue).status(),
                     Status::InvalidArgument);
    }

    medtest::section("Opções do driver iio - nomes de atributo, nunca caminhos");

    {
        // An option can reach this device's own sysfs directory and nothing
        // else: a '/' or a '.' would let a configuration file write anywhere
        // under /sys.
        const char* const badNames[] = {"../../power/state", "buffer/enable", "Hardwaregain",
                                        "hardware-gain", "in_voltage0.scale", ""};
        for (const char* name : badNames) {
            DeviceConfig config = baseConfig("iio");
            config.driverOptions[name] = "1";
            CHECK_MSG(MedicalDeviceFactory::create(config).status() == Status::InvalidArgument,
                      std::string("iio aceitou o nome de atributo '") + name + "'");
        }

        // One setting, one source: the rate comes from sampleRateHz.
        DeviceConfig rate = baseConfig("iio");
        rate.driverOptions["sampling_frequency"] = "500";
        CHECK_STATUS(MedicalDeviceFactory::create(rate).status(), Status::InvalidArgument);

        DeviceConfig empty = baseConfig("iio");
        empty.driverOptions["hardwaregain"] = "";
        CHECK_STATUS(MedicalDeviceFactory::create(empty).status(), Status::InvalidArgument);

        DeviceConfig newline = baseConfig("iio");
        newline.driverOptions["hardwaregain"] = "24\n1";
        CHECK_STATUS(MedicalDeviceFactory::create(newline).status(), Status::InvalidArgument);

        DeviceConfig valid = baseConfig("iio");
        valid.driverOptions["hardwaregain"] = "24";
        valid.driverOptions["test_signal"] = "off";
        CHECK(MedicalDeviceFactory::create(valid).isOk());
    }

    medtest::section("Driver simulado - a aritmética do conversor");

    {
        // The options the EEG recipe derives for its simulated link. The
        // numbers are that application's, and they are here only as a
        // realistic case: 24 bits over +-187500 uV.
        const double limit = 187500.0;
        const double step = limit / 8388608.0;  // signed 24 bit

        // 22.3517 nV. The value is here in microvolts and is the number the
        // frame header's comment argues about: an integer nanovolt field could
        // only say 22, a 1.6% gain error on every sample of every trace.
        CHECK_NEAR(step * 1000.0, 22.3517, 0.001);

        DeviceConfig config = baseConfig("simulated");
        config.driverOptions["unit"] = "uV";
        config.driverOptions["full_scale"] = "187500";
        config.driverOptions["resolution_bits"] = "24";
        config.driverOptions["noise_rms"] = "0.14";
        config.driverOptions["tones"] = "20@10@0.4, 5@50@0, 2@0.3@0.4";

        Result<std::unique_ptr<MedicalDevice>> created = MedicalDeviceFactory::create(config);
        CHECK(created.isOk());
        if (!created.isOk()) {
            return;
        }
        std::unique_ptr<MedicalDevice> device = created.take();

        const DeviceInfo info = device->info();
        CHECK_EQ(info.driver, std::string("simulated"));
        CHECK_EQ(info.unit, std::string("uV"));
        CHECK_EQ(static_cast<long long>(info.channelCount), 8LL);
        CHECK_NEAR(info.sampleRateHz, 250.0, 1e-9);

        CHECK_STATUS(device->selfTest(), Status::Ok);

        // read() before start() is Unavailable, not an empty frame: a consumer
        // that forgot to start must not receive something that looks like data.
        CHECK(!device->read(std::chrono::milliseconds(0)).isOk());
        CHECK(!device->isRunning());

        CHECK_STATUS(device->start(), Status::Ok);
        CHECK(device->isRunning());
        CHECK_STATUS(device->start(), Status::Ok);  // idempotent

        std::uint64_t previousSequence = 0;
        int framesRead = 0;
        bool sawSignal = false;
        for (int attempt = 0; attempt < 6 && framesRead < 3; ++attempt) {
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(500));
            if (!frame.isOk()) {
                continue;
            }
            ++framesRead;
            const SampleFrame& f = frame.value();

            CHECK_EQ(static_cast<long long>(f.channelCount), 8LL);
            CHECK_EQ(static_cast<long long>(f.samplesPerChannel), 14LL);
            CHECK_EQ(static_cast<long long>(f.samples.size()), 8LL * 14LL);

            // Sequence numbers are contiguous. A gap is how a record says it
            // lost something, so a driver that skipped one silently would make
            // the record lie by omission.
            CHECK_EQ(static_cast<long long>(f.sequence),
                     static_cast<long long>(previousSequence + 1));
            previousSequence = f.sequence;

            for (std::size_t i = 0; i < f.samples.size(); ++i) {
                const double value = f.samples[i];
                sawSignal = sawSignal || std::fabs(value) > 1.0;

                // Every sample is an exact multiple of the converter's step:
                // a value between two codes is one no such converter produces.
                const double codes = value / step;
                CHECK_MSG(std::fabs(codes - std::floor(codes + 0.5)) < 1e-3,
                          "amostra fora da grade do LSB: " + std::to_string(value));

                // And nothing exceeds full scale.
                CHECK_MSG(std::fabs(value) <= limit + step,
                          "amostra além do fundo de escala: " + std::to_string(value));
            }

            // The bounds-checked accessor returns 0 rather than reading past
            // the buffer.
            CHECK_EQ(f.at(99, 0), 0.0F);
            CHECK_EQ(f.at(0, 9999), 0.0F);
            CHECK_EQ(f.at(0, 0), f.samples[0]);
        }
        CHECK_MSG(framesRead >= 3, "o simulador não entregou três quadros");
        CHECK_MSG(sawSignal, "os tons configurados não apareceram no sinal");

        CHECK_STATUS(device->stop(), Status::Ok);
        CHECK(!device->isRunning());
    }

    {
        // Saturation, actually exercised rather than assumed. A full scale this
        // small puts the tone's amplitude outside it, so the clipping branch
        // runs - and what it must produce is a saturated sample, which an
        // operator recognises, never a wrapped one, which looks like a signal
        // and is not.
        const double limit = 4.5;
        DeviceConfig config = baseConfig("simulated");
        config.driverOptions["full_scale"] = "4.5";
        config.driverOptions["resolution_bits"] = "24";
        config.driverOptions["tones"] = "20@10";

        Result<std::unique_ptr<MedicalDevice>> created = MedicalDeviceFactory::create(config);
        CHECK(created.isOk());
        if (!created.isOk()) {
            return;
        }
        std::unique_ptr<MedicalDevice> device = created.take();
        CHECK_STATUS(device->start(), Status::Ok);

        bool sawSaturation = false;
        bool sawOverflow = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(500));
            if (!frame.isOk()) {
                continue;
            }
            for (const float sample : frame.value().samples) {
                if (std::fabs(sample) > limit * 1.001) {
                    sawOverflow = true;
                }
                if (std::fabs(std::fabs(sample) - limit) < limit * 1e-3) {
                    sawSaturation = true;
                }
            }
        }
        CHECK_MSG(sawSaturation, "a saturação nunca foi exercitada por este teste");
        CHECK_MSG(!sawOverflow, "uma amostra passou do fundo de escala");
        device->stop();
    }

    {
        // The square wave replaces the tones on every channel, at exactly the
        // configured amplitude when there is no noise and the amplitude sits on
        // the grid.
        const double amplitude = 1875.0;

        DeviceConfig config = baseConfig("simulated");
        config.driverOptions["full_scale"] = "187500";
        config.driverOptions["tones"] = "20@10";
        config.driverOptions["square"] = "1875@0.9765625";

        Result<std::unique_ptr<MedicalDevice>> created = MedicalDeviceFactory::create(config);
        CHECK(created.isOk());
        if (!created.isOk()) {
            return;
        }
        std::unique_ptr<MedicalDevice> device = created.take();
        CHECK_STATUS(device->selfTest(), Status::Ok);
        CHECK_STATUS(device->start(), Status::Ok);

        bool any = false;
        bool offLevel = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            Result<SampleFrame> frame = device->read(std::chrono::milliseconds(500));
            if (!frame.isOk()) {
                continue;
            }
            for (const float sample : frame.value().samples) {
                any = true;
                offLevel = offLevel || std::fabs(std::fabs(sample) - amplitude) > 1e-3;
            }
        }
        CHECK(any);
        CHECK_MSG(!offLevel, "a onda quadrada saiu de +-amplitude (ou os tons vazaram)");
        device->stop();
    }

    medtest::section("Formatos de fio do enlace AMP");

    {
        // Layout is ABI: the two sides are compiled by different toolchains for
        // different architectures. The static_asserts in the header already
        // fail the build, and these exist so that a failure is also reported as
        // a failed test rather than only as a compile error.
        CHECK_EQ(static_cast<long long>(sizeof(amp::FrameHeader)), 40LL);
        CHECK_EQ(static_cast<long long>(sizeof(amp::ControlOption)), 56LL);
        CHECK_EQ(static_cast<long long>(sizeof(amp::ControlMessage)), 464LL);
        CHECK_EQ(static_cast<long long>(sizeof(amp::ControlAck)), 72LL);

        // 464 bytes is not a taste: a stock rpmsg buffer is 512 with about 496
        // usable, and a ninth option would not fit. Needing one is a protocol
        // change, not a bigger struct.
        CHECK(sizeof(amp::ControlMessage) <= 496);
        CHECK_EQ(static_cast<long long>(amp::kControlMaxOptions), 8LL);

        // The magics are the ASCII the comments claim, which is what someone
        // reading a hex dump on a bench will be looking for.
        CHECK_EQ(static_cast<long long>(amp::kFrameMagic), 0x4D454547LL);      // "MEEG"
        CHECK_EQ(static_cast<long long>(amp::kControlMagic), 0x4D435452LL);    // "MCTR"
        CHECK_EQ(static_cast<long long>(amp::kControlAckMagic), 0x4D435441LL); // "MCTA"
    }

    {
        // CRC-32 against the standard check value.
        //
        // This is the only vector in the suite whose source is OUTSIDE this
        // repository: 0xCBF43926 is the published check value of CRC-32
        // (IEEE 802.3, reflected, poly 0xEDB88320) for the ASCII string
        // "123456789". That matters more than it looks - a test written from
        // the same reading that wrote the implementation proves consistency,
        // not correctness. This one proves the firmware on the other core,
        // written against the same standard, will compute the same number.
        const char* const vector = "123456789";
        CHECK_EQ(static_cast<long long>(amp::crc32(vector, 9)), 0xCBF43926LL);

        // Empty input is the identity of the construction, and a frame with no
        // payload must not be given a garbage checksum.
        CHECK_EQ(static_cast<long long>(amp::crc32("", 0)), 0LL);

        // A single flipped bit changes the result: the property the whole
        // discard-on-mismatch rule depends on.
        const unsigned char clean[] = {0x4D, 0x45, 0x45, 0x47, 0x01, 0x02, 0x03, 0x04};
        unsigned char flipped[sizeof(clean)];
        std::memcpy(flipped, clean, sizeof(clean));
        flipped[5] ^= 0x01;
        CHECK(amp::crc32(clean, sizeof(clean)) != amp::crc32(flipped, sizeof(flipped)));
    }
}
