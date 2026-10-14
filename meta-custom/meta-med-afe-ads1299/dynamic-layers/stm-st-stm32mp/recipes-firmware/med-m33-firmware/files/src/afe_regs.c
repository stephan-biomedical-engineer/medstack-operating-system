/* SPDX-License-Identifier: MIT */
/* The converter's register arithmetic - see afe_regs.h. */

#include "afe_regs.h"

/* CONFIG1 [Table 13]: bit 7 reserved 1, bits 4:3 reserved 2h -> 0x90 | DR.
 * ODR = f_CLK >> (7 + DR): DR = 6 is 2.048 MHz / 8192 = 250 SPS, and each step
 * down doubles it, to DR = 0 at 16000. DR = 7 is reserved. */
#define CONFIG1_RESERVED 0x90u
#define CONFIG1_DR_250   6

/* CONFIG2 [Table 14]: bits 7:5 reserved 6h, bit 3 reserved 0 -> 0xC0.
 * INT_CAL (bit 4) routes the internal test source; CAL_AMP (bit 2) = 0 is 1x,
 * i.e. VREF / 2400 peak; CAL_FREQ (1:0) = 0 is f_CLK / 2^21. */
#define CONFIG2_RESERVED 0xC0u
#define CONFIG2_INT_CAL  0x10u

/* CONFIG3 [Table 15]: bits 6:5 reserved 3h; bit 7 enables the internal
 * reference buffer (the datasheet's PD_REFBUF, inverted polarity). */
#define CONFIG3_RESERVED 0x60u
#define CONFIG3_REFBUF   0x80u

/* CONFIG3, the bias amplifier [as ti-ads1299.c's set_bias()]: BIASREF_INT
 * (bit 3) feeds its reference from (AVDD + AVSS) / 2, PD_BIAS (bit 2) powers it
 * - the name is inverted like PD_REFBUF's, 1 is ON. */
#define CONFIG3_BIASREF_INT 0x08u
#define CONFIG3_PD_BIAS     0x04u

/* CONFIG4: PD_LOFF_COMP (bit 1), inverted name again, 1 powers the lead-off
 * comparators on. SINGLE_SHOT (bit 3) stays 0: continuous conversion. */
#define CONFIG4_PD_LOFF_COMP 0x02u

/* MISC1 [Figure 64]: SRB1 (bit 5) ties the SRB1 pin to every inverting input.
 * Every other bit is reserved 0, and the reset value is 00h. */
#define MISC1_SRB1 0x20u

/* LOFF at its reset value: COMP_TH 95 %/5 %, ILEAD_OFF 6 nA, FLEAD_OFF DC.
 * Written, not left alone, so that the read-back covers it. */
#define LOFF_RESET 0x00u

/* CHnSET: PGA in 6:4, MUX in 2:0 (0 electrodes, 5 test signal), power-down
 * bit 7 and SRB2 bit 3 left 0. */
#define CHSET_PGA_SHIFT  4
#define CHSET_MUX_NORMAL 0x00u
#define CHSET_MUX_SHORT  0x01u /* both inputs to (VREFP + VREFN) / 2, off the pins [9.3.1.1.1] */
#define CHSET_MUX_TEMP   0x04u /* the on-chip temperature sensor's diodes */
#define CHSET_MUX_TEST   0x05u

static const double kGains[] = {1, 2, 4, 6, 8, 12, 24};

int afe_pga_field(double gain)
{
    int i;
    for (i = 0; i < (int)(sizeof(kGains) / sizeof(kGains[0])); ++i) {
        if (gain == kGains[i]) {
            return i;
        }
    }
    return -1;
}

int afe_rate_field(uint32_t rate_hz)
{
    int field;
    for (field = CONFIG1_DR_250; field >= 0; --field) {
        if (rate_hz == (250u << (CONFIG1_DR_250 - field))) {
            return field;
        }
    }
    return -1;
}

double afe_sensor_gain(double gain)
{
    return gain > 12.0 ? 12.0 : gain;
}

void afe_registers(int rate_field, int pga_field, int test_signal, int bias_drive,
                   int lead_off, int referential, int shorted, int temperature,
                   struct afe_regs *r)
{
    const uint8_t mux = test_signal ? CHSET_MUX_TEST
                      : shorted ? CHSET_MUX_SHORT
                      : CHSET_MUX_NORMAL;
    /* The channels that have electrodes on their pins. */
    const uint8_t electrodes = (uint8_t)(
        AFE_CHANNEL_MASK & ~(temperature ? (1u << AFE_TEMPERATURE_CHANNEL) : 0u));
    /* The inverting inputs: one per channel, or one node on the SRB1 pin. */
    const uint8_t negative = referential ? AFE_REFERENCE_BIT : electrodes;
    unsigned c;

    r->config1 = (uint8_t)(CONFIG1_RESERVED | ((unsigned)rate_field & 0x07u));
    r->config2 = (uint8_t)(CONFIG2_RESERVED | (test_signal ? CONFIG2_INT_CAL : 0u));
    r->config3 = (uint8_t)(CONFIG3_RESERVED | CONFIG3_REFBUF |
                           (bias_drive ? CONFIG3_BIASREF_INT | CONFIG3_PD_BIAS : 0u));
    for (c = 0; c < AFE_CHANNELS; ++c) {
        r->chset[c] = (uint8_t)(((unsigned)pga_field << CHSET_PGA_SHIFT) | mux);
    }
    if (temperature) {
        const int sensor_field = afe_pga_field(afe_sensor_gain(kGains[pga_field]));
        r->chset[AFE_TEMPERATURE_CHANNEL] =
            (uint8_t)(((unsigned)sensor_field << CHSET_PGA_SHIFT) | CHSET_MUX_TEMP);
    }
    r->loff = LOFF_RESET;
    r->sens[0] = bias_drive ? electrodes : 0u; /* BIAS_SENSP */
    r->sens[1] = (bias_drive && !referential) ? electrodes : 0u; /* BIAS_SENSN */
    r->sens[2] = lead_off ? electrodes : 0u;   /* LOFF_SENSP */
    r->sens[3] = lead_off ? negative : 0u;     /* LOFF_SENSN */
    r->misc1 = referential ? MISC1_SRB1 : 0u;
    r->config4 = lead_off ? CONFIG4_PD_LOFF_COMP : 0u;
}

void afe_status_lead_off(const uint8_t *status, uint8_t *positive, uint8_t *negative)
{
    *positive = (uint8_t)(((status[0] & 0x0Fu) << 4) | (status[1] >> 4));
    *negative = (uint8_t)(((status[1] & 0x0Fu) << 4) | (status[2] >> 4));
}

int32_t afe_code24(const uint8_t *b)
{
    uint32_t raw = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
    if (raw & 0x800000u) {
        raw |= 0xFF000000u;
    }
    return (int32_t)raw;
}

int32_t afe_code_to_nv_exact(int32_t code, uint32_t gain)
{
    /* AFE_VREF_UV in nanovolts, and gain * 2^23: at most 24 * 2^23, which
     * fits 32 bits, under a product of at most 2^23 * 4.5e9, which fits 64. */
    const uint32_t divisor = gain << 23;
    const int negative = code < 0;
    /* |code| is at most 2^31, which an unsigned 32 bits holds even for the
     * most negative int32. */
    const uint32_t absolute = negative ? 0u - (uint32_t)code : (uint32_t)code;
    const uint64_t magnitude = (uint64_t)absolute * UINT64_C(4500000000);
    const uint64_t rounded = (magnitude + divisor / 2u) / divisor;

    if (rounded > 2147483647ull) {
        return negative ? (int32_t)-2147483647 - 1 : 2147483647;
    }
    return negative ? -(int32_t)rounded : (int32_t)rounded;
}

int32_t afe_code_to_nv(int32_t code, double gain, double vref_uv)
{
    const double lsb_nv = vref_uv * 1000.0 / (gain * 8388608.0);
    const double nv = (double)code * lsb_nv;
    /* Saturate rather than wrap. The producer refuses the gains whose full
     * scale does not fit (afe_gain_fits_frame), so this never fires on a
     * prescription it accepted; it is here so that a mistake elsewhere becomes
     * a clipped trace and not a sample with the opposite sign. */
    if (nv >= 2147483647.0) {
        return 2147483647;
    }
    if (nv <= -2147483648.0) {
        return (int32_t)-2147483647 - 1;
    }
    return (int32_t)(nv < 0 ? nv - 0.5 : nv + 0.5);
}

int afe_gain_fits_frame(double gain, double vref_uv)
{
    /* Full scale is +-VREF / gain; the frame carries int32 nanovolts. */
    return vref_uv * 1000.0 / gain <= 2147483647.0;
}

int afe_status_valid(const uint8_t *status)
{
    return (status[0] & 0xF0u) == 0xC0u;
}
