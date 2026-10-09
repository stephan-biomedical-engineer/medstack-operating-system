// SPDX-License-Identifier: MIT
//
// Host tests for the Cortex-M33 producer (med_producer.c).
//
// What a pass here means: the producer answers prescriptions, builds frames
// and generates its signal as med_producer.h promises, compiled from the file
// the firmware recipe ships. What it does NOT mean: anything about timing,
// rpmsg, the mailbox or the board - main.c is not compiled here - and the
// float arithmetic of the host's libm is not newlib's, which is why signal
// values are compared against an independent double-precision oracle with a
// stated tolerance and never byte-for-byte.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "med_producer.h"
#include "afe_regs.h"

static int checks;
static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        ++checks;                                                     \
        if (!(cond)) {                                                \
            ++failures;                                               \
            printf("   FALHA  %s:%d  %s\n          ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

static void section(const char *name) { printf("\n-- %s\n", name); }

/* ------------------------------------------------------------- messages */

struct option_spec {
    const char *key;
    const char *value;
};

/* A control message as encodeControl() in MedicalDevice.cpp builds it. */
static med_amp_control_message make_control(const struct option_spec *options, unsigned count)
{
    med_amp_control_message m;
    unsigned i;
    memset(&m, 0, sizeof(m));
    m.magic = MED_AMP_CONTROL_MAGIC;
    m.version = MED_AMP_CONTROL_VERSION;
    m.optionCount = (uint16_t)count;
    for (i = 0; i < count; ++i) {
        strncpy(m.options[i].key, options[i].key, sizeof(m.options[i].key) - 1);
        strncpy(m.options[i].value, options[i].value, sizeof(m.options[i].value) - 1);
    }
    m.crc32 = med_amp_crc32(m.options, count * sizeof(m.options[0]));
    return m;
}

/* The five keys eeg.conf prescribes, in the sorted order the recipe sends them
 * (do_derive_device_options, link == 'amp'), with the two this producer cannot
 * honour set to what it CAN honour. */
static const struct option_spec kHonourable[] = {
    {"afe.bias_drive", "false"},
    {"afe.gain", "24"},
    {"afe.lead_off_detection", "false"},
    {"afe.reference_uv", "4500000"},
    {"afe.test_signal", "off"},
};

/* The prescription eeg.conf actually ships. */
static const struct option_spec kShipped[] = {
    {"afe.bias_drive", "true"},
    {"afe.gain", "24"},
    {"afe.lead_off_detection", "true"},
    {"afe.reference_uv", "4500000"},
    {"afe.test_signal", "off"},
};

static int send_control(struct med_producer *p, const med_amp_control_message *m,
                        med_amp_control_ack *ack)
{
    memset(ack, 0xA5, sizeof(*ack));
    return med_producer_on_control(p, m, sizeof(*m), ack);
}

static int nul_terminated(const char *s, size_t size) { return memchr(s, '\0', size) != NULL; }

/* ------------------------------------------------------- the oracle */

/* The signal as med_producer.c's comment specifies it, in double precision,
 * from first principles - not from the producer's code. */
static double oracle_nv(int internal, double gain, double reference_uv, unsigned long long n,
                        unsigned channel)
{
    const double pi = 3.14159265358979323846;
    double v;
    double full = reference_uv / gain * 1000.0;
    if (internal) {
        double amplitude = reference_uv / 2400.0 * 1000.0;
        v = (n % 256 < 128) ? amplitude : -amplitude;
    } else {
        double t = (double)n / 250.0;
        v = 20000.0 * sin(2 * pi * 10.0 * t + 2 * pi * channel / 8.0) +
            5000.0 * sin(2 * pi * 50.0 * t);
    }
    if (v > full) v = full;
    if (v < -full) v = -full;
    return v;
}

/* ------------------------------------------------------------- tests */

static void test_prescription(void)
{
    struct med_producer p;
    med_amp_control_ack ack;
    med_amp_control_message m;

    section("prescrição: o que o produtor aceita");

    med_producer_init(&p);
    m = make_control(kHonourable, 5);
    CHECK(send_control(&p, &m, &ack) == 1, "uma prescrição válida tem de ter resposta");
    CHECK(ack.magic == MED_AMP_CONTROL_ACK_MAGIC, "magic 0x%08x", ack.magic);
    CHECK(ack.version == MED_AMP_CONTROL_VERSION, "versão %u", ack.version);
    CHECK(ack.rejectedIndex == 0, "rejectedIndex %u, detail '%.64s'", ack.rejectedIndex, ack.detail);
    CHECK(nul_terminated(ack.detail, sizeof(ack.detail)), "detail sem NUL");
    CHECK(strstr(ack.detail, "synthetic") != NULL,
          "um aceite sintético tem de dizer que é sintético: '%.64s'", ack.detail);
    CHECK(p.streaming == 1, "aceite e não transmitindo");

    /* The prescription eeg.conf ships asks for lead-off and bias drive. */
    med_producer_init(&p);
    m = make_control(kShipped, 5);
    CHECK(send_control(&p, &m, &ack) == 1, "recusa também é resposta");
    CHECK(ack.rejectedIndex == 1, "esperado recusar a opção 0 (afe.bias_drive), recusou %u",
          ack.rejectedIndex);
    CHECK(strstr(ack.detail, "afe.bias_drive") != NULL && strstr(ack.detail, "no electrodes"),
          "detail não diz qual nem por quê: '%.64s'", ack.detail);
    CHECK(p.streaming == 0, "recusada e transmitindo");

    /* With bias drive off, lead-off is the first thing it cannot do. */
    {
        struct option_spec o[5];
        memcpy(o, kShipped, sizeof(o));
        o[0].value = "false";
        m = make_control(o, 5);
        send_control(&p, &m, &ack);
        CHECK(ack.rejectedIndex == 3, "esperado recusar a opção 2 (lead_off), recusou %u",
              ack.rejectedIndex);
    }

    /* An empty prescription is a valid one: defaults, and stream. */
    med_producer_init(&p);
    m = make_control(NULL, 0);
    CHECK(send_control(&p, &m, &ack) == 1 && ack.rejectedIndex == 0 && p.streaming,
          "prescrição vazia: rejectedIndex %u streaming %d", ack.rejectedIndex, p.streaming);

    section("prescrição: cada valor que tem de ser recusado");
    {
        static const struct {
            const char *key;
            const char *value;
            const char *why;
        } bad[] = {
            {"afe.gain", "", "vazio"},
            {"afe.gain", "0", "zero"},
            {"afe.gain", "-24", "negativo"},
            {"afe.gain", "24x", "lixo no fim"},
            {"afe.gain", " 24", "espaço"},
            {"afe.gain", "1e3", "expoente"},
            {"afe.gain", "0x18", "hexadecimal"},
            {"afe.gain", "2.4.1", "dois pontos"},
            {"afe.reference_uv", "abc", "não numérico"},
            {"afe.test_signal", "on", "nem off nem internal"},
            {"afe.test_signal", "OFF", "maiúsculas"},
            {"afe.lead_off_detection", "1", "booleano não literal"},
            {"afe.bias_drive", "yes", "booleano não literal"},
            {"afe.lead_off_detection", "true", "não há eletrodo"},
            {"afe.notch", "50", "chave desconhecida"},
            {"gain", "24", "sem o prefixo"},
        };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            struct option_spec o[2] = {{"afe.reference_uv", "4500000"}, {bad[i].key, bad[i].value}};
            med_producer_init(&p);
            m = make_control(o, 2);
            send_control(&p, &m, &ack);
            CHECK(ack.rejectedIndex == 2 && p.streaming == 0,
                  "%s = '%s' (%s): rejectedIndex %u, streaming %d", bad[i].key, bad[i].value,
                  bad[i].why, ack.rejectedIndex, p.streaming);
        }
    }
    {
        struct option_spec o[2] = {{"afe.gain", "24"}, {"afe.gain", "12"}};
        med_producer_init(&p);
        m = make_control(o, 2);
        send_control(&p, &m, &ack);
        CHECK(ack.rejectedIndex == 2 && strstr(ack.detail, "twice"),
              "chave repetida: rejectedIndex %u '%.64s'", ack.rejectedIndex, ack.detail);
    }
    {
        /* A key that fills its field with no NUL: the CRC is right, the
         * message is well-formed, the option is not. */
        med_producer_init(&p);
        m = make_control(kHonourable, 1);
        memset(m.options[0].key, 'k', sizeof(m.options[0].key));
        m.crc32 = med_amp_crc32(m.options, sizeof(m.options[0]));
        send_control(&p, &m, &ack);
        CHECK(ack.rejectedIndex == 1, "chave sem NUL: rejectedIndex %u", ack.rejectedIndex);
    }

    section("prescrição: mensagens malformadas não têm resposta");
    {
        struct {
            const char *what;
            void (*mutate)(med_amp_control_message *);
            size_t length;
        } cases[5];
        size_t i;
        med_amp_control_message base = make_control(kHonourable, 5);

        /* Each case starts from a streaming producer, to show a malformed
         * message neither answers nor disturbs the session in progress. */
        for (i = 0; i < 5; ++i) {
            med_amp_control_message bad = base;
            size_t length = sizeof(bad);
            const char *what = "";
            med_producer_init(&p);
            send_control(&p, &base, &ack);
            switch (i) {
            case 0: bad.magic ^= 1u; what = "magic"; break;
            case 1: bad.version = 2; what = "versão"; break;
            case 2: bad.crc32 ^= 1u; what = "CRC"; break;
            case 3: bad.optionCount = 9; what = "9 opções"; break;
            case 4: length = sizeof(bad) - 1; what = "um byte a menos"; break;
            }
            CHECK(med_producer_on_control(&p, &bad, length, &ack) == 0,
                  "%s: houve resposta", what);
            CHECK(p.controls_malformed == 1, "%s: malformed = %u", what, p.controls_malformed);
            CHECK(p.streaming == 1, "%s: a sessão em curso parou", what);
        }
        (void)cases;
    }
}

static void test_frames(void)
{
    struct med_producer p;
    med_amp_control_ack ack;
    med_amp_control_message m = make_control(kHonourable, 5);
    uint8_t frame[MED_PRODUCER_FRAME_BYTES];
    med_amp_frame_header h;
    int32_t payload[MED_PRODUCER_CHANNELS * MED_PRODUCER_SAMPLES_PER_FRAME];
    unsigned i, f, c;
    int completed;

    section("quadros: geometria e cabeçalho");

    CHECK(MED_PRODUCER_FRAME_BYTES == 496, "%zu bytes", (size_t)MED_PRODUCER_FRAME_BYTES);
    CHECK(MED_PRODUCER_FRAME_BYTES <= 496, "não cabe num buffer rpmsg de 512");

    med_producer_init(&p);
    CHECK(med_producer_on_sample(&p, 0, frame) == 0, "transmitiu sem prescrição");
    send_control(&p, &m, &ack);

    for (f = 0; f < 3; ++f) {
        completed = 0;
        for (i = 0; i < MED_PRODUCER_SAMPLES_PER_FRAME; ++i) {
            uint64_t t = 1000000ull + (f * MED_PRODUCER_SAMPLES_PER_FRAME + i) * 4000ull;
            int r = med_producer_on_sample(&p, t, frame);
            if (i + 1 < MED_PRODUCER_SAMPLES_PER_FRAME) {
                CHECK(r == 0, "quadro %u completou na amostra %u", f, i);
            }
            completed = r;
        }
        CHECK(completed == 1, "quadro %u não completou em 14 amostras", f);

        memcpy(&h, frame, sizeof(h));
        memcpy(payload, frame + sizeof(h), sizeof(payload));
        CHECK(h.magic == MED_AMP_FRAME_MAGIC, "magic 0x%08x", h.magic);
        CHECK(h.version == MED_AMP_FRAME_VERSION, "versão %u", h.version);
        CHECK(h.channelCount == 8 && h.samplesPerChannel == 14, "%u x %u", h.channelCount,
              h.samplesPerChannel);
        CHECK(h.sampleRateMilliHz == 250000, "%u mHz", h.sampleRateMilliHz);
        CHECK(h.sequence == f, "sequência %llu no quadro %u", (unsigned long long)h.sequence, f);
        CHECK(h.timestampMicros == 1000000ull + f * 14 * 4000ull,
              "carimbo %llu: tem de ser o da PRIMEIRA amostra do quadro",
              (unsigned long long)h.timestampMicros);
        CHECK(h.scaleNanoUnitsPerLsb == 1, "escala %d", h.scaleNanoUnitsPerLsb);
        CHECK(h.crc32 == med_amp_crc32(frame + sizeof(h), sizeof(payload) + sizeof(med_amp_lead_off)),
              "CRC (amostras + lead-off) não fecha no quadro %u", f);
        {
            med_amp_lead_off lo;
            memcpy(&lo, frame + sizeof(h) + sizeof(payload), sizeof(lo));
            CHECK(lo.monitoredPositive == 0 && lo.monitoredNegative == 0 && lo.offPositive == 0 &&
                      lo.offNegative == 0,
                  "sintético declarou eletrodo monitorado: %04x %04x %04x %04x",
                  lo.monitoredPositive, lo.monitoredNegative, lo.offPositive, lo.offNegative);
        }

        /* Interleaving and content: sample i of channel c at i * 8 + c, and
         * equal to the producer's own published function of (n, c). */
        {
            int mismatches = 0;
            for (i = 0; i < 14; ++i) {
                for (c = 0; c < 8; ++c) {
                    uint64_t n = (uint64_t)f * 14 + i;
                    if (payload[i * 8 + c] != med_producer_sample_nv(&p, n, c)) {
                        ++mismatches;
                    }
                }
            }
            CHECK(mismatches == 0, "%d amostras fora do lugar no quadro %u", mismatches, f);
        }
    }

    section("quadros: uma nova prescrição reinicia a sessão");
    for (i = 0; i < 5; ++i) {
        med_producer_on_sample(&p, 9000000, frame); /* a frame partly filled */
    }
    send_control(&p, &m, &ack);
    completed = 0;
    for (i = 0; i < 14; ++i) {
        completed = med_producer_on_sample(&p, 5000000ull + i * 4000ull, frame);
    }
    memcpy(&h, frame, sizeof(h));
    CHECK(completed == 1 && h.sequence == 0 && h.timestampMicros == 5000000ull,
          "depois da nova prescrição: completou %d, sequência %llu, carimbo %llu - "
          "amostras da sessão anterior vazaram", completed, (unsigned long long)h.sequence,
          (unsigned long long)h.timestampMicros);

    section("quadros: uma prescrição recusada para a transmissão");
    {
        med_amp_control_message bad = make_control(kShipped, 5);
        send_control(&p, &bad, &ack);
        completed = 0;
        for (i = 0; i < 28; ++i) {
            completed |= med_producer_on_sample(&p, i * 4000ull, frame);
        }
        CHECK(completed == 0, "transmitiu sob uma prescrição recusada");
    }
}

static void test_signal(void)
{
    struct med_producer p;
    double worst = 0.0;
    unsigned long long n;
    unsigned c;

    section("sinal: contra um oráculo independente em precisão dupla");

    med_producer_init(&p); /* gain 24, reference 4.5 V, test signal off */
    for (n = 0; n < 6400; ++n) {
        for (c = 0; c < 8; ++c) {
            double e = fabs(med_producer_sample_nv(&p, n, c) - oracle_nv(0, 24, 4500000, n, c));
            if (e > worst) worst = e;
        }
    }
    /* float sinf at amplitude 20000 nV: a few ULPs plus rounding to int. */
    CHECK(worst <= 3.0, "erro máximo %.2f nV contra o oráculo (tolerância 3 nV)", worst);

    {
        /* Every pair of channels must differ somewhere in one period. Compared
         * as sequences and not at one instant: at n = 0 channels 0 and 4 sit at
         * phases 0 and pi, both of which are a zero of the sine. */
        unsigned c1, c2, identical = 0;
        for (c1 = 0; c1 < 8; ++c1) {
            for (c2 = c1 + 1; c2 < 8; ++c2) {
                int same = 1;
                for (n = 0; n < 25 && same; ++n) {
                    same = med_producer_sample_nv(&p, n, c1) == med_producer_sample_nv(&p, n, c2);
                }
                identical += (unsigned)same;
            }
        }
        CHECK(identical == 0, "%u pares de canais idênticos num período: uma troca seria invisível",
              identical);
    }

    section("sinal: não degrada com o tempo");
    {
        /* lcm(25, 5, 256) = 6400: after any multiple of it the signal must
         * repeat exactly. 2^40 samples is 139 years at 250 SPS - if phase were
         * kept in float, this is where it would show. */
        const unsigned long long far = 6400ull * (1ull << 34);
        int differ = 0;
        for (n = 0; n < 6400; ++n) {
            for (c = 0; c < 8; ++c) {
                if (med_producer_sample_nv(&p, far + n, c) != med_producer_sample_nv(&p, n, c)) {
                    ++differ;
                }
            }
        }
        CHECK(differ == 0, "%d amostras diferentes depois de 2^40 amostras", differ);
    }

    section("sinal: o sinal de teste interno");
    {
        med_amp_control_ack ack;
        struct option_spec o[1] = {{"afe.test_signal", "internal"}};
        med_amp_control_message m = make_control(o, 1);
        int wrong = 0;
        med_producer_init(&p);
        send_control(&p, &m, &ack);
        CHECK(p.test_signal == MED_TEST_SIGNAL_INTERNAL, "não ligou o sinal de teste");
        for (n = 0; n < 1024; ++n) {
            for (c = 0; c < 8; ++c) {
                int32_t expected = (n % 256 < 128) ? 1875000 : -1875000; /* 4.5 V / 2400 */
                if (med_producer_sample_nv(&p, n, c) != expected) ++wrong;
            }
        }
        CHECK(wrong == 0, "%d amostras fora de +-1875000 nV, período 256", wrong);
    }

    section("sinal: o que excede o fundo de escala é ceifado");
    {
        med_amp_control_ack ack;
        /* 4.5 V / 400000 = 11.25 uV of full scale, below the 25 uV peak. */
        struct option_spec o[1] = {{"afe.gain", "400000"}};
        med_amp_control_message m = make_control(o, 1);
        int32_t peak = 0;
        med_producer_init(&p);
        send_control(&p, &m, &ack);
        for (n = 0; n < 25; ++n) {
            int32_t v = med_producer_sample_nv(&p, n, 0);
            if (abs(v) > peak) peak = abs(v);
        }
        CHECK(peak == 11250, "pico %d nV, esperado o fundo de escala 11250", peak);
    }
}

static void test_converter_registers(void)
{
    struct afe_regs r;
    static const struct { double gain; int field; } gains[] = {
        {1, 0}, {2, 1}, {4, 2}, {6, 3}, {8, 4}, {12, 5}, {24, 6},
        {3, -1}, {0, -1}, {48, -1}, {24.5, -1}, {16, -1},
    };
    size_t i;

    section("conversor: registradores (valores do datasheet, como o driver do kernel)");

    for (i = 0; i < sizeof(gains) / sizeof(gains[0]); ++i) {
        CHECK(afe_pga_field(gains[i].gain) == gains[i].field, "ganho %g -> campo %d, esperado %d",
              gains[i].gain, afe_pga_field(gains[i].gain), gains[i].field);
    }
    /* Reset values of CONFIG1 (0x96) and CONFIG2 (0xC0) ARE the 250 SPS and
     * no-test-signal images - and the bridge bench read those reset values
     * from the part (BRINGUP_AFE.md §4.3). */
    afe_registers(6, 0, 0, 0, &r);
    CHECK(r.config1 == 0x96 && r.config2 == 0xC0 && r.config3 == 0xE0 && r.chset == 0x60,
          "eletrodos, ganho 24: %02x %02x %02x %02x, esperado 96 c0 e0 60",
          r.config1, r.config2, r.config3, r.chset);
    CHECK(r.loff == 0 && r.sens[0] == 0 && r.sens[1] == 0 && r.sens[2] == 0 && r.sens[3] == 0 &&
              r.config4 == 0,
          "sem bias nem lead-off, algo ligado: LOFF %02x SENS %02x %02x %02x %02x CONFIG4 %02x",
          r.loff, r.sens[0], r.sens[1], r.sens[2], r.sens[3], r.config4);
    afe_registers(6, 1, 0, 0, &r);
    CHECK(r.config2 == 0xD0 && r.chset == 0x65,
          "sinal de teste, ganho 24: CONFIG2 %02x CHnSET %02x, esperado d0 65", r.config2, r.chset);
    afe_registers(2, 0, 0, 0, &r);
    CHECK(r.chset == 0x20, "ganho 4: CHnSET %02x, esperado 20", r.chset);

    /* Bias, derived: ti-ads1299.c's set_bias(DERIVED) writes BIAS_SENSP and
     * BIAS_SENSN = GENMASK(channels - 1, 0) and sets BIASREF_INT | PD_BIAS
     * (CONFIG3 bits 3 and 2) - 0xE0 | 0x0C. */
    afe_registers(6, 0, 1, 0, &r);
    CHECK(r.config3 == 0xEC && r.sens[0] == 0xFF && r.sens[1] == 0xFF && r.sens[2] == 0 &&
              r.sens[3] == 0 && r.config4 == 0,
          "bias derivado: CONFIG3 %02x SENS %02x %02x %02x %02x CONFIG4 %02x, esperado ec ff ff 00 00 00",
          r.config3, r.sens[0], r.sens[1], r.sens[2], r.sens[3], r.config4);
    /* Lead-off: the comparators (CONFIG4 bit 1, as ti-ads1299.c) AND the
     * per-channel enables the driver leaves at 0. LOFF at its reset value. */
    afe_registers(6, 0, 0, 1, &r);
    CHECK(r.config3 == 0xE0 && r.sens[0] == 0 && r.sens[1] == 0 && r.sens[2] == 0xFF &&
              r.sens[3] == 0xFF && r.config4 == 0x02 && r.loff == 0x00,
          "lead-off: CONFIG3 %02x SENS %02x %02x %02x %02x CONFIG4 %02x LOFF %02x",
          r.config3, r.sens[0], r.sens[1], r.sens[2], r.sens[3], r.config4, r.loff);

    section("conversor: amostras de 24 bits");
    {
        static const struct { uint8_t b[3]; int32_t v; } codes[] = {
            {{0x00, 0x00, 0x00}, 0},        {{0x00, 0x00, 0x01}, 1},
            {{0x7f, 0xff, 0xff}, 8388607},  {{0x80, 0x00, 0x00}, -8388608},
            {{0xff, 0xff, 0xff}, -1},       {{0x01, 0x47, 0xae}, 83886},
        };
        for (i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
            CHECK(afe_code24(codes[i].b) == codes[i].v, "%02x%02x%02x -> %d, esperado %d",
                  codes[i].b[0], codes[i].b[1], codes[i].b[2], afe_code24(codes[i].b), codes[i].v);
        }
    }

    section("conversor: escala em nanovolts");
    {
        /* Oracle: LSB = VREF / (gain * 2^23), from the datasheet's transfer
         * function, computed here independently of afe_regs.c. */
        static const struct { int32_t code; double gain; } v[] = {
            {1, 24}, {-1, 24}, {8388607, 24}, {-8388608, 24}, {83886, 24}, {1000000, 4},
        };
        for (i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
            double expect = (double)v[i].code * 4.5e9 / (v[i].gain * 8388608.0);
            double got = afe_code_to_nv(v[i].code, v[i].gain, AFE_VREF_UV);
            CHECK(fabs(got - expect) <= 0.5, "código %d, ganho %g: %.0f nV, esperado %.2f",
                  v[i].code, v[i].gain, got, expect);
        }
        /* The test signal: +-VREF/2400 = 1.875 mV is 83886 codes at gain 24,
         * the theoretical value the bridge measured 83542..83829 against. */
        CHECK(abs(afe_code_to_nv(83886, 24, AFE_VREF_UV) - 1875000) <= 5,
              "83886 códigos = %d nV, esperado ~1875000", afe_code_to_nv(83886, 24, AFE_VREF_UV));
        /* Saturation, not wrap, where the format cannot carry the value. */
        CHECK(afe_code_to_nv(8388607, 1, AFE_VREF_UV) == 2147483647, "ganho 1, +FS não saturou");
        CHECK(afe_code_to_nv(-8388608, 1, AFE_VREF_UV) == (int32_t)-2147483647 - 1,
              "ganho 1, -FS não saturou");
        CHECK(!afe_gain_fits_frame(1, AFE_VREF_UV) && !afe_gain_fits_frame(2, AFE_VREF_UV) &&
                  afe_gain_fits_frame(4, AFE_VREF_UV) && afe_gain_fits_frame(24, AFE_VREF_UV),
              "quais ganhos cabem em int32 nV: só 4 em diante");
    }

    section("conversor: palavra de status");
    {
        uint8_t ok[3] = {0xC0, 0x00, 0x00}, ok2[3] = {0xCF, 0xFF, 0xFF};
        uint8_t ff[3] = {0xFF, 0xFF, 0xFF}, zero[3] = {0, 0, 0}, d0[3] = {0xD0, 0, 0};
        CHECK(afe_status_valid(ok) && afe_status_valid(ok2), "status 1100.... recusado");
        CHECK(!afe_status_valid(ff), "0xFF (MISO solto) aceito como status");
        CHECK(!afe_status_valid(zero) && !afe_status_valid(d0), "status sem 1100 aceito");
    }
    {
        /* The lead-off bits, against a word assembled here from the layout's
         * description - 1100, STATP[7:0], STATN[7:0], GPIO[7:4] - as one
         * 24-bit integer, not with afe_regs.c's byte arithmetic. Both come
         * from the same [SBAS499?] reading, so this proves the extraction is
         * consistent with the description; whether the description is the
         * part's is what afe.c's check against LOFF_STAT answers on the
         * board. */
        static const uint8_t pn[][2] = {{0, 0}, {0xFF, 0}, {0, 0xFF}, {0xA5, 0x3C}, {0x01, 0x80},
                                        {0x80, 0x01}, {0xFF, 0xFF}};
        unsigned gpio;
        for (i = 0; i < sizeof(pn) / sizeof(pn[0]); ++i) {
            for (gpio = 0; gpio < 16; gpio += 15) {
                uint32_t word = 0xC00000u | ((uint32_t)pn[i][0] << 12) | ((uint32_t)pn[i][1] << 4) | gpio;
                uint8_t b[3] = {(uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word};
                uint8_t gp, gn;
                afe_status_lead_off(b, &gp, &gn);
                CHECK(gp == pn[i][0] && gn == pn[i][1] && afe_status_valid(b),
                      "status %02x%02x%02x: P %02x N %02x, esperado %02x %02x", b[0], b[1], b[2],
                      gp, gn, pn[i][0], pn[i][1]);
            }
        }
    }
}

static void test_converter_source(void)
{
    struct med_producer p;
    med_amp_control_ack ack;
    med_amp_control_message m;
    uint8_t frame[MED_PRODUCER_FRAME_BYTES];
    int32_t nv[MED_PRODUCER_CHANNELS];
    unsigned i, c;

    section("fonte conversor: a prescrição é julgada pelo que a peça tem");

    med_producer_init(&p);
    med_producer_set_source(&p, MED_SOURCE_CONVERTER, NULL);
    m = make_control(kHonourable, 5);
    send_control(&p, &m, &ack);
    CHECK(ack.rejectedIndex == 0 && p.streaming && strstr(ack.detail, "converter"),
          "aceitável recusada: %u '%.64s'", ack.rejectedIndex, ack.detail);
    {
        static const struct { const char *k, *v, *why; } bad[] = {
            {"afe.gain", "3", "1, 2, 4"},
            {"afe.gain", "2", "int32"},
            {"afe.gain", "1", "int32"},
            {"afe.reference_uv", "3000000", "internal"},
        };
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            struct option_spec o[1] = {{bad[i].k, bad[i].v}};
            med_producer_init(&p);
            med_producer_set_source(&p, MED_SOURCE_CONVERTER, NULL);
            m = make_control(o, 1);
            send_control(&p, &m, &ack);
            CHECK(ack.rejectedIndex == 1 && !p.streaming && strstr(ack.detail, bad[i].why),
                  "%s = %s: rejectedIndex %u '%.64s'", bad[i].k, bad[i].v, ack.rejectedIndex,
                  ack.detail);
        }
    }

    section("fonte conversor: o gerador sintético não alimenta quadros");
    med_producer_init(&p);
    med_producer_set_source(&p, MED_SOURCE_CONVERTER, NULL);
    m = make_control(kHonourable, 5);
    send_control(&p, &m, &ack);
    {
        int any = 0;
        for (i = 0; i < 28; ++i) any |= med_producer_on_sample(&p, i * 4000ull, frame);
        CHECK(!any, "on_sample (sintético) produziu quadro com fonte conversor");
    }
    {
        int completed = 0, mismatched = 0;
        int32_t payload[MED_PRODUCER_CHANNELS * MED_PRODUCER_SAMPLES_PER_FRAME];
        for (i = 0; i < 14; ++i) {
            for (c = 0; c < 8; ++c) nv[c] = (int32_t)(i * 1000 + c);
            completed = med_producer_on_values(&p, 7000000ull + i * 4000ull, nv, 0, 0, frame);
        }
        memcpy(payload, frame + sizeof(med_amp_frame_header), sizeof(payload));
        for (i = 0; i < 14; ++i)
            for (c = 0; c < 8; ++c)
                mismatched += payload[i * 8 + c] != (int32_t)(i * 1000 + c);
        CHECK(completed && mismatched == 0, "on_values: completou %d, %d valores fora do lugar",
              completed, mismatched);
    }

    section("fonte conversor: bias e lead-off");
    med_producer_init(&p);
    med_producer_set_source(&p, MED_SOURCE_CONVERTER, NULL);
    m = make_control(kShipped, 5);
    send_control(&p, &m, &ack);
    CHECK(ack.rejectedIndex == 0 && p.streaming && p.bias_drive == 1 && p.lead_off == 1,
          "a prescrição de fábrica recusada pelo conversor: %u '%.64s' bias %d lead-off %d",
          ack.rejectedIndex, ack.detail, p.bias_drive, p.lead_off);
    {
        /* Lead-off watches electrodes; on the test signal there are none. */
        struct option_spec o[5];
        memcpy(o, kShipped, sizeof(o));
        o[4].value = "internal";
        m = make_control(o, 5);
        send_control(&p, &m, &ack);
        CHECK(ack.rejectedIndex == 3 && !p.streaming && strstr(ack.detail, "afe.lead_off_detection") &&
                  strstr(ack.detail, "test signal"),
              "lead-off com sinal de teste: rejectedIndex %u '%.64s'", ack.rejectedIndex, ack.detail);
        o[0].value = "true";
        o[2].value = "false";
        m = make_control(o, 5);
        send_control(&p, &m, &ack);
        CHECK(ack.rejectedIndex == 0 && p.bias_drive == 1 && p.lead_off == 0,
              "bias com sinal de teste recusado: %u '%.64s'", ack.rejectedIndex, ack.detail);
    }
    {
        /* The lead-off block of a frame: monitored = every channel, off = the
         * OR over its conversions, and nothing above the eighth channel. */
        med_amp_frame_header h;
        med_amp_lead_off lo;
        int completed = 0;
        m = make_control(kShipped, 5);
        send_control(&p, &m, &ack);
        for (c = 0; c < 8; ++c) nv[c] = 0;
        for (i = 0; i < 14; ++i) {
            uint16_t op = i == 3 ? 0x0004u : i == 9 ? 0xFF01u : 0u;
            uint16_t on = i == 13 ? 0x0080u : 0u;
            completed = med_producer_on_values(&p, i, nv, op, on, frame);
        }
        memcpy(&h, frame, sizeof(h));
        memcpy(&lo, frame + sizeof(h) + MED_PRODUCER_CHANNELS * MED_PRODUCER_SAMPLES_PER_FRAME * 4,
               sizeof(lo));
        CHECK(completed && lo.monitoredPositive == 0x00FF && lo.monitoredNegative == 0x00FF &&
                  lo.offPositive == 0x0005 && lo.offNegative == 0x0080,
              "bloco de lead-off: monitored %04x %04x off %04x %04x, esperado 00ff 00ff 0005 0080",
              lo.monitoredPositive, lo.monitoredNegative, lo.offPositive, lo.offNegative);
        /* The next frame starts clean: a detachment does not outlive its frame. */
        for (i = 0; i < 14; ++i) completed = med_producer_on_values(&p, i, nv, 0, 0, frame);
        memcpy(&lo, frame + sizeof(h) + MED_PRODUCER_CHANNELS * MED_PRODUCER_SAMPLES_PER_FRAME * 4,
               sizeof(lo));
        CHECK(completed && lo.offPositive == 0 && lo.offNegative == 0,
              "o eletrodo solto do quadro anterior vazou: off %04x %04x", lo.offPositive,
              lo.offNegative);
        /* The CRC covers the block: one bit flipped there must not close. */
        memcpy(&h, frame, sizeof(h));
        frame[MED_PRODUCER_FRAME_BYTES - 1] ^= 0x01u;
        CHECK(h.crc32 != med_amp_crc32(frame + sizeof(h), MED_PRODUCER_FRAME_BYTES - sizeof(h)),
              "um bit trocado no bloco de lead-off passou pelo CRC");
    }
    {
        /* Detection not prescribed: the bits a conversion carries are dropped,
         * and the frame says nothing was monitored. */
        med_amp_lead_off lo;
        int completed = 0;
        m = make_control(kHonourable, 5);
        send_control(&p, &m, &ack);
        for (i = 0; i < 14; ++i) completed = med_producer_on_values(&p, i, nv, 0xFF, 0xFF, frame);
        memcpy(&lo, frame + sizeof(med_amp_frame_header) +
                        MED_PRODUCER_CHANNELS * MED_PRODUCER_SAMPLES_PER_FRAME * 4,
               sizeof(lo));
        CHECK(completed && lo.monitoredPositive == 0 && lo.monitoredNegative == 0 &&
                  lo.offPositive == 0 && lo.offNegative == 0,
              "sem lead-off prescrito: %04x %04x %04x %04x", lo.monitoredPositive,
              lo.monitoredNegative, lo.offPositive, lo.offNegative);
    }

    section("fonte conversor: o hardware que não aceita vira recusa");
    med_producer_init(&p);
    med_producer_set_source(&p, MED_SOURCE_CONVERTER, NULL);
    m = make_control(kHonourable, 5);
    send_control(&p, &m, &ack);
    med_producer_refuse(&p, &ack, "the converter did not take the settings");
    CHECK(ack.rejectedIndex == 1 && !p.streaming && strstr(ack.detail, "did not take"),
          "refuse(): rejectedIndex %u streaming %d '%.64s'", ack.rejectedIndex, p.streaming,
          ack.detail);

    section("fonte ausente: tudo é recusado, nada transmite");
    med_producer_init(&p);
    med_producer_set_source(&p, MED_SOURCE_ABSENT, "no converter answered on SPI6");
    m = make_control(kHonourable, 5);
    send_control(&p, &m, &ack);
    CHECK(ack.rejectedIndex == 1 && !p.streaming && strstr(ack.detail, "absent") &&
              strstr(ack.detail, "SPI6"),
          "ausente aceitou ou não disse por quê: %u '%.64s'", ack.rejectedIndex, ack.detail);
    m = make_control(NULL, 0);
    send_control(&p, &m, &ack);
    CHECK(ack.rejectedIndex == 1 && !p.streaming, "ausente aceitou a prescrição vazia");
    {
        int any = 0;
        for (c = 0; c < 8; ++c) nv[c] = 0;
        for (i = 0; i < 28; ++i) any |= med_producer_on_values(&p, i, nv, 0, 0, frame);
        any |= med_producer_on_sample(&p, 0, frame);
        CHECK(!any, "ausente produziu quadro");
    }
}

int main(void)
{
    printf("Produtor do M33 - verificações de host\n");
    test_prescription();
    test_frames();
    test_signal();
    test_converter_registers();
    test_converter_source();
    printf("\n%d verificações, %d falhas\n", checks, failures);
    printf("\nNão coberto por esta suíte:\n"
           "  - main.c: o relógio, a interrupção, o mailbox e o rpmsg (só na placa)\n"
           "  - a libm do newlib: o sinal é comparado com tolerância, não byte a byte\n");
    return failures == 0 ? 0 : 1;
}
