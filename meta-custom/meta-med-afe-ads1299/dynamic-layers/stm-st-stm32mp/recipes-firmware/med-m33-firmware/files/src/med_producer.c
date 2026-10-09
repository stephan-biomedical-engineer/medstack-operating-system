/* SPDX-License-Identifier: MIT */
/*
 * The producer side of the AMP link - see med_producer.h.
 */

#include "med_producer.h"
#include "afe_regs.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- signal */

/*
 * The synthetic signal, input-referred, in nanovolts.
 *
 * test_signal = off: on every channel a 10 Hz tone of 20 uV - the size and
 * band of an alpha rhythm - plus a 5 uV 50 Hz tone, the mains interference
 * every real recording carries. Channel c is the 10 Hz tone shifted by c/8 of
 * a turn, so that two channels swapped anywhere along the path are visible as
 * a phase error instead of looking identical.
 *
 * test_signal = internal: what a converter of this class produces from its own
 * test source, on every channel at once - a square wave of +-VREF/2400 at
 * f_CLK / 2^21 = 2.048 MHz / 2097152 = 0.9765625 Hz, which at 250 SPS is a
 * period of exactly 256 samples. Here VREF is the prescription's
 * reference_uv, so the amplitude follows the prescription the way the
 * converter's would.
 *
 * Phases are computed from n modulo the period, in integers, before any float
 * is involved: 2*pi*10*n/250 in single precision would lose its fractional part
 * within hours of acquisition, and a slowly degrading synthetic signal is a
 * defect that looks like physiology.
 */
#define TONE_A_PERIOD  25u     /* 10 Hz at 250 SPS */
#define TONE_A_NV      20000.0f
#define TONE_B_PERIOD  5u      /* 50 Hz at 250 SPS */
#define TONE_B_NV      5000.0f
#define SQUARE_PERIOD  256u    /* 0.9765625 Hz at 250 SPS */

static const float kTwoPi = 6.28318530717958647692f;

int32_t med_producer_sample_nv(const struct med_producer *p, uint64_t n, uint32_t channel)
{
    double value;
    double full_scale_nv = p->reference_uv / p->gain * 1000.0;

    if (p->test_signal == MED_TEST_SIGNAL_INTERNAL) {
        double amplitude_nv = p->reference_uv / 2400.0 * 1000.0;
        value = ((n % SQUARE_PERIOD) < SQUARE_PERIOD / 2) ? amplitude_nv : -amplitude_nv;
    } else {
        float a = (float)(n % TONE_A_PERIOD) / (float)TONE_A_PERIOD +
                  (float)channel / (float)MED_PRODUCER_CHANNELS;
        float b = (float)(n % TONE_B_PERIOD) / (float)TONE_B_PERIOD;
        value = (double)(TONE_A_NV * sinf(kTwoPi * a) + TONE_B_NV * sinf(kTwoPi * b));
    }

    /* What a converter cannot represent, it clips; so does this. */
    if (value > full_scale_nv) {
        value = full_scale_nv;
    } else if (value < -full_scale_nv) {
        value = -full_scale_nv;
    }
    return (int32_t)(value < 0 ? value - 0.5 : value + 0.5);
}

/* ------------------------------------------------------------ prescription */

/*
 * A positive decimal number: digits, an optional point and more digits,
 * nothing else. Hand-written rather than strtod because the firmware has no
 * locale and should not acquire one to read "24", and because what it refuses
 * is then exactly what this function says - no exponents, no signs, no
 * leading or trailing space, no hex.
 */
static int parse_positive(const char *text, double *out)
{
    double value = 0.0;
    double scale = 0.1;
    int digits = 0;
    int point = 0;

    for (; *text; ++text) {
        if (*text >= '0' && *text <= '9') {
            if (point) {
                value += (*text - '0') * scale;
                scale /= 10.0;
            } else {
                value = value * 10.0 + (*text - '0');
            }
            ++digits;
        } else if (*text == '.' && !point) {
            point = 1;
        } else {
            return 0;
        }
    }
    if (digits == 0 || value <= 0.0) {
        return 0;
    }
    *out = value;
    return 1;
}

static int parse_boolean(const char *text, int *out)
{
    if (strcmp(text, "true") == 0) {
        *out = 1;
        return 1;
    }
    if (strcmp(text, "false") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static void set_detail(med_amp_control_ack *ack, const char *a, const char *b)
{
    size_t used = 0;
    const char *parts[2];
    size_t i;

    parts[0] = a;
    parts[1] = b;
    memset(ack->detail, 0, sizeof(ack->detail));
    for (i = 0; i < 2 && parts[i]; ++i) {
        size_t length = strlen(parts[i]);
        if (length > sizeof(ack->detail) - 1 - used) {
            length = sizeof(ack->detail) - 1 - used;
        }
        memcpy(ack->detail + used, parts[i], length);
        used += length;
    }
}

static int reject(struct med_producer *p, med_amp_control_ack *ack, uint16_t index,
                  const char *key, const char *why)
{
    /* streaming is already 0: med_producer_on_control() stops it before any
     * verdict, which is the one place that has to happen. */
    ack->rejectedIndex = (uint16_t)(index + 1u);
    set_detail(ack, key, why);
    ++p->controls_rejected;
    return 1;
}

void med_producer_init(struct med_producer *p)
{
    memset(p, 0, sizeof(*p));
    p->gain = 24.0;
    p->reference_uv = 4500000.0;
    p->test_signal = MED_TEST_SIGNAL_OFF;
    p->source = MED_SOURCE_SYNTHETIC;
}

void med_producer_set_source(struct med_producer *p, enum med_source source,
                             const char *absent_reason)
{
    p->source = source;
    p->absent_reason = absent_reason ? absent_reason : "no reason recorded";
    p->streaming = 0;
}

void med_producer_refuse(struct med_producer *p, med_amp_control_ack *ack, const char *why)
{
    p->streaming = 0;
    ++p->controls_rejected;
    if (p->controls_accepted) {
        --p->controls_accepted;
    }
    ack->rejectedIndex = 1;
    set_detail(ack, why, NULL);
}

int med_producer_on_control(struct med_producer *p, const void *data, size_t length,
                            med_amp_control_ack *ack)
{
    med_amp_control_message message;
    double gain = 24.0;
    double reference_uv = 4500000.0;
    enum med_test_signal test_signal = MED_TEST_SIGNAL_OFF;
    int bias_drive = 0;
    int lead_off = 0;
    uint16_t lead_off_index = 0;
    unsigned seen = 0;
    uint16_t i;

    enum { SEEN_GAIN = 1, SEEN_REF = 2, SEEN_TEST = 4, SEEN_LEADOFF = 8, SEEN_BIAS = 16 };

    if (length != sizeof(message)) {
        ++p->controls_malformed;
        return 0;
    }
    memcpy(&message, data, sizeof(message));
    if (message.magic != MED_AMP_CONTROL_MAGIC ||
        message.version != MED_AMP_CONTROL_VERSION ||
        message.optionCount > MED_AMP_CONTROL_MAX_OPTIONS ||
        message.crc32 != med_amp_crc32(message.options,
                                       message.optionCount * sizeof(message.options[0]))) {
        ++p->controls_malformed;
        return 0;
    }

    /*
     * From here on the message is what it claims to be, and every outcome is
     * an answer. Streaming stops now, before the answer is built: whatever the
     * verdict, no frame of the old session may follow it.
     */
    p->streaming = 0;
    memset(ack, 0, sizeof(*ack));
    ack->magic = MED_AMP_CONTROL_ACK_MAGIC;
    ack->version = MED_AMP_CONTROL_VERSION;

    /* No converter where one was expected: nothing is accepted, whatever it
     * says. Index 0 even for an empty prescription - there is no option to
     * blame, and the detail says what is missing. */
    if (p->source == MED_SOURCE_ABSENT) {
        return reject(p, ack, 0, "front-end absent: ", p->absent_reason);
    }

    for (i = 0; i < message.optionCount; ++i) {
        med_amp_control_option *option = &message.options[i];
        const char *key = option->key;
        const char *value = option->value;
        unsigned bit;
        int flag;

        if (memchr(option->key, '\0', sizeof(option->key)) == NULL ||
            memchr(option->value, '\0', sizeof(option->value)) == NULL) {
            return reject(p, ack, i, "option", " is not NUL terminated");
        }

        if (strcmp(key, "afe.gain") == 0) {
            bit = SEEN_GAIN;
            if (!parse_positive(value, &gain)) {
                return reject(p, ack, i, key, ": not a positive number");
            }
            /* The converter has seven gains and nothing between them; a gain
             * it cannot set is refused, never rounded to a neighbour. */
            if (p->source == MED_SOURCE_CONVERTER && afe_pga_field(gain) < 0) {
                return reject(p, ack, i, key, ": not 1, 2, 4, 6, 8, 12 or 24");
            }
            /* A gain the part has, whose full scale the frame cannot carry. */
            if (p->source == MED_SOURCE_CONVERTER && !afe_gain_fits_frame(gain, AFE_VREF_UV)) {
                return reject(p, ack, i, key, ": full scale exceeds int32 nanovolts");
            }
        } else if (strcmp(key, "afe.reference_uv") == 0) {
            bit = SEEN_REF;
            if (!parse_positive(value, &reference_uv)) {
                return reject(p, ack, i, key, ": not a positive number");
            }
            /* The board has the internal 4.5 V reference and no other. A
             * prescription for another one cannot be met by configuration. */
            if (p->source == MED_SOURCE_CONVERTER && reference_uv != AFE_VREF_UV) {
                return reject(p, ack, i, key, ": this board has only the internal 4500000");
            }
        } else if (strcmp(key, "afe.test_signal") == 0) {
            bit = SEEN_TEST;
            if (strcmp(value, "off") == 0) {
                test_signal = MED_TEST_SIGNAL_OFF;
            } else if (strcmp(value, "internal") == 0) {
                test_signal = MED_TEST_SIGNAL_INTERNAL;
            } else {
                return reject(p, ack, i, key, ": not off or internal");
            }
        } else if (strcmp(key, "afe.lead_off_detection") == 0 ||
                   strcmp(key, "afe.bias_drive") == 0) {
            /*
             * The converter honours both: the derived bias amplifier, and DC
             * lead-off detection on every input with the result carried to
             * Linux in every frame (med_amp_lead_off). A synthetic producer
             * honours neither - there is no electrode to detect the loss of
             * and no body to drive - and refuses "true", because accepting it
             * would put a lead-off monitor and a driven electrode in the
             * record of a session that had neither.
             */
            bit = (key[4] == 'l') ? SEEN_LEADOFF : SEEN_BIAS;
            if (!parse_boolean(value, &flag)) {
                return reject(p, ack, i, key, ": not true or false");
            }
            if (flag && p->source != MED_SOURCE_CONVERTER) {
                return reject(p, ack, i, key, ": synthetic producer, no electrodes");
            }
            if (bit == SEEN_LEADOFF) {
                lead_off = flag;
                lead_off_index = i;
            } else {
                bias_drive = flag;
            }
        } else {
            return reject(p, ack, i, key, ": unknown to this producer");
        }

        if (seen & bit) {
            return reject(p, ack, i, key, ": given twice");
        }
        seen |= bit;
    }

    /*
     * Lead-off detection watches the electrodes. With every channel switched
     * to the internal test signal there is no electrode in the measurement,
     * and "monitored, all attached" would be a statement about inputs nobody
     * was recording from. Refused, and the option blamed is the detection.
     */
    if (lead_off && test_signal == MED_TEST_SIGNAL_INTERNAL) {
        return reject(p, ack, lead_off_index, "afe.lead_off_detection",
                      ": on the test signal, no electrodes");
    }

    p->gain = gain;
    p->reference_uv = reference_uv;
    p->test_signal = test_signal;
    p->bias_drive = bias_drive;
    p->lead_off = lead_off;
    p->sequence = 0;
    p->filled = 0;
    p->streaming = 1;
    ++p->controls_accepted;

    /* Accepted. The detail is not read by the receiver on success; it is there
     * for whoever dumps the exchange, so that a synthetic session cannot be
     * mistaken for a measured one by someone holding only the bytes. */
    ack->rejectedIndex = 0;
    set_detail(ack, p->source == MED_SOURCE_CONVERTER
                        ? "accepted; converter configured and read back"
                        : "accepted by a synthetic producer, no converter",
               NULL);
    return 1;
}

/* ------------------------------------------------------------------ frames */

int med_producer_on_sample(struct med_producer *p, uint64_t now_us, uint8_t *frame)
{
    const uint64_t base = p->sequence * MED_PRODUCER_SAMPLES_PER_FRAME;
    int32_t nv[MED_PRODUCER_CHANNELS];
    uint32_t c;

    if (!p->streaming || p->source != MED_SOURCE_SYNTHETIC) {
        return 0;
    }
    for (c = 0; c < MED_PRODUCER_CHANNELS; ++c) {
        nv[c] = med_producer_sample_nv(p, base + p->filled, c);
    }
    return med_producer_on_values(p, now_us, nv, 0u, 0u, frame);
}

int med_producer_on_values(struct med_producer *p, uint64_t now_us, const int32_t *nv,
                           uint16_t off_positive, uint16_t off_negative, uint8_t *frame)
{
    const uint16_t monitored = p->lead_off ? (uint16_t)((1u << MED_PRODUCER_CHANNELS) - 1u) : 0u;
    med_amp_frame_header header;
    med_amp_lead_off contact;

    if (!p->streaming) {
        return 0;
    }

    if (p->filled == 0) {
        p->first_us = now_us;
        p->off_positive = 0;
        p->off_negative = 0;
    }
    p->off_positive |= off_positive;
    p->off_negative |= off_negative;
    memcpy(&p->samples[p->filled * MED_PRODUCER_CHANNELS], nv,
           MED_PRODUCER_CHANNELS * sizeof(nv[0]));
    if (++p->filled < MED_PRODUCER_SAMPLES_PER_FRAME) {
        return 0;
    }

    memset(&header, 0, sizeof(header));
    header.magic = MED_AMP_FRAME_MAGIC;
    header.version = MED_AMP_FRAME_VERSION;
    header.channelCount = MED_PRODUCER_CHANNELS;
    header.samplesPerChannel = MED_PRODUCER_SAMPLES_PER_FRAME;
    header.sampleRateMilliHz = MED_PRODUCER_RATE_HZ * 1000u;
    header.sequence = p->sequence;
    header.timestampMicros = p->first_us;
    header.scaleNanoUnitsPerLsb = 1; /* the samples ARE nanovolts */

    /* "off" only where monitored: a bit for an electrode nobody checked is
     * not a measurement, and the receiver should never have to mask it. */
    contact.monitoredPositive = monitored;
    contact.monitoredNegative = monitored;
    contact.offPositive = (uint16_t)(p->off_positive & monitored);
    contact.offNegative = (uint16_t)(p->off_negative & monitored);

    memcpy(frame + sizeof(header), p->samples, sizeof(p->samples));
    memcpy(frame + sizeof(header) + sizeof(p->samples), &contact, sizeof(contact));
    /* The CRC covers the samples and the lead-off block, as laid out. */
    header.crc32 = med_amp_crc32(frame + sizeof(header), sizeof(p->samples) + sizeof(contact));
    memcpy(frame, &header, sizeof(header));

    ++p->sequence;
    p->filled = 0;
    return 1;
}
