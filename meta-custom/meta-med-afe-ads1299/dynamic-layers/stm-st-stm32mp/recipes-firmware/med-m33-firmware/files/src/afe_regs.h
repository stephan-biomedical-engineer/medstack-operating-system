/* SPDX-License-Identifier: MIT */
/*
 * The converter's register arithmetic, with no hardware in it.
 *
 * Which bytes a prescription becomes, and what a 24-bit sample means in
 * nanovolts, decided here in plain C so that tests/m33-producer can check both
 * on the host. afe.c owns the SPI transfers that carry these bytes.
 *
 * Every constant is the one ti-ads1299.c (linux-med) uses, which the bridge
 * bench checked against the part on 2026-10-04 (BRINGUP_AFE.md §4): reset
 * values read back register by register, and the test signal's amplitude
 * measured. Reserved fields are written with their mandated VALUE, not with a
 * mask of ones - CONFIG2's is 6h over three bits.
 */

#ifndef AFE_REGS_H
#define AFE_REGS_H

#include <stdint.h>

#define AFE_CHANNELS        8u
#define AFE_FRAME_BYTES     27u        /* RDATAC: 3 status + 8 x 3 data */
#define AFE_VREF_UV         4500000.0  /* internal reference, the only one this board has */

/* Opcodes and register addresses [SBAS499, as ti-ads1299.c]. */
#define AFE_CMD_WAKEUP  0x02u
#define AFE_CMD_RESET   0x06u
#define AFE_CMD_START   0x08u
#define AFE_CMD_STOP    0x0au
#define AFE_CMD_RDATAC  0x10u
#define AFE_CMD_SDATAC  0x11u
#define AFE_CMD_RDATA   0x12u
#define AFE_CMD_RREG    0x20u
#define AFE_CMD_WREG    0x40u

#define AFE_REG_ID      0x00u
#define AFE_REG_CONFIG1 0x01u
#define AFE_REG_CONFIG2 0x02u
#define AFE_REG_CONFIG3 0x03u
#define AFE_REG_LOFF    0x04u
#define AFE_REG_CH1SET  0x05u
#define AFE_REG_BIAS_SENSP 0x0du /* then BIAS_SENSN, LOFF_SENSP, LOFF_SENSN */
#define AFE_REG_LOFF_STATP 0x12u /* then LOFF_STATN */
#define AFE_REG_CONFIG4 0x17u

struct afe_regs {
    uint8_t config1;
    uint8_t config2;
    uint8_t config3;
    uint8_t chset; /* written to all eight channels */
    uint8_t loff;
    /* BIAS_SENSP, BIAS_SENSN, LOFF_SENSP, LOFF_SENSN: four consecutive
     * registers, one bit per channel each. */
    uint8_t sens[4];
    uint8_t config4;
};

/* The PGA field for a gain, or -1 if the part has no such gain. The gains are
 * {1, 2, 4, 6, 8, 12, 24}, indexed by the field - not the ADS1298's table. */
int afe_pga_field(double gain);

/* The register image for 250 SPS, internal reference, the given PGA field, and
 * either every channel on its electrode inputs or every channel on the
 * internal test signal (1x amplitude, f_CLK / 2^21).
 *
 * bias_drive: the bias amplifier drives the inverse of the average of every
 * channel's inputs - "derived", the arrangement ti-ads1299.c's set_bias() calls
 * by that name, with the same registers.
 *
 * lead_off: DC lead-off detection on both inputs of every channel, comparators
 * on, LOFF at its reset value (thresholds 95 %/5 %, 6 nA, DC - the values
 * ti-ads1299.c also leaves). Unlike that driver, the per-channel enables are
 * set too: comparators with no channel enabled detect nothing (its own
 * comment above LOFF_SENSP says so). */
void afe_registers(int pga_field, int test_signal, int bias_drive, int lead_off,
                   struct afe_regs *r);

/* The channels this producer has: one bit each, the mask the per-channel
 * registers and the lead-off block use. */
#define AFE_CHANNEL_MASK ((uint8_t)((1u << AFE_CHANNELS) - 1u))

/*
 * The lead-off bits an RDATAC/RDATA frame carries in its 24-bit status word:
 * 1100, LOFF_STATP[7:0], LOFF_STATN[7:0], GPIO[7:4]. [SBAS499?] - this layout
 * has no oracle in the kernel driver, which reads lead-off from the registers.
 * afe.c checks it against those registers every time detection is enabled, so
 * a wrong layout fails the prescription instead of reporting the wrong
 * electrodes.
 */
void afe_status_lead_off(const uint8_t *status, uint8_t *positive, uint8_t *negative);

/* A 24-bit big-endian two's-complement sample, sign-extended. */
int32_t afe_code24(const uint8_t *b);

/* Input-referred nanovolts for a code: one LSB is VREF / (gain * 2^23),
 * rounded to the nearest nanovolt. */
int32_t afe_code_to_nv(int32_t code, double gain, double vref_uv);

/* Whether the full scale at this gain, +-VREF / gain, fits the frame format's
 * int32 nanovolts (+-2.147 V). With the 4.5 V reference, gains 1 and 2 do not:
 * a sample near full scale would wrap to the opposite sign. */
int afe_gain_fits_frame(double gain, double vref_uv);

/* The status word of an RDATAC frame starts with the nibble 1100. Anything
 * else means the frame was read out of step with the converter. */
int afe_status_valid(const uint8_t *status);

#endif /* AFE_REGS_H */
