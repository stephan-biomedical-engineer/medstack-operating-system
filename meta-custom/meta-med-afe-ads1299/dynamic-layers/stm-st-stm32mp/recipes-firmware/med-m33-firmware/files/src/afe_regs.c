/* SPDX-License-Identifier: MIT */
/* The converter's register arithmetic - see afe_regs.h. */

#include "afe_regs.h"

/* CONFIG1 [Table 13]: bit 7 reserved 1, bits 4:3 reserved 2h -> 0x90 | DR.
 * DR = 6 is 250 SPS: ODR = f_CLK >> (7 + DR) = 2.048 MHz / 8192. */
#define CONFIG1_RESERVED 0x90u
#define CONFIG1_DR_250   0x06u

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

/* LOFF at its reset value: COMP_TH 95 %/5 %, ILEAD_OFF 6 nA, FLEAD_OFF DC.
 * Written, not left alone, so that the read-back covers it. */
#define LOFF_RESET 0x00u

/* CHnSET: PGA in 6:4, MUX in 2:0 (0 electrodes, 5 test signal), power-down
 * bit 7 and SRB2 bit 3 left 0. */
#define CHSET_PGA_SHIFT  4
#define CHSET_MUX_NORMAL 0x00u
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

void afe_registers(int pga_field, int test_signal, int bias_drive, int lead_off,
                   struct afe_regs *r)
{
    r->config1 = (uint8_t)(CONFIG1_RESERVED | CONFIG1_DR_250);
    r->config2 = (uint8_t)(CONFIG2_RESERVED | (test_signal ? CONFIG2_INT_CAL : 0u));
    r->config3 = (uint8_t)(CONFIG3_RESERVED | CONFIG3_REFBUF |
                           (bias_drive ? CONFIG3_BIASREF_INT | CONFIG3_PD_BIAS : 0u));
    r->chset = (uint8_t)(((unsigned)pga_field << CHSET_PGA_SHIFT) |
                         (test_signal ? CHSET_MUX_TEST : CHSET_MUX_NORMAL));
    r->loff = LOFF_RESET;
    r->sens[0] = bias_drive ? AFE_CHANNEL_MASK : 0u; /* BIAS_SENSP */
    r->sens[1] = bias_drive ? AFE_CHANNEL_MASK : 0u; /* BIAS_SENSN */
    r->sens[2] = lead_off ? AFE_CHANNEL_MASK : 0u;   /* LOFF_SENSP */
    r->sens[3] = lead_off ? AFE_CHANNEL_MASK : 0u;   /* LOFF_SENSN */
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
