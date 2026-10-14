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
#define AFE_REG_MISC1   0x15u
#define AFE_REG_CONFIG4 0x17u

struct afe_regs {
    uint8_t config1;
    uint8_t config2;
    uint8_t config3;
    uint8_t chset[AFE_CHANNELS]; /* CH1SET..CH8SET */
    uint8_t loff;
    /* BIAS_SENSP, BIAS_SENSN, LOFF_SENSP, LOFF_SENSN: four consecutive
     * registers, one bit per channel each. */
    uint8_t sens[4];
    uint8_t misc1;
    uint8_t config4;
};

/* The PGA field for a gain, or -1 if the part has no such gain. The gains are
 * {1, 2, 4, 6, 8, 12, 24}, indexed by the field - not the ADS1298's table. */
int afe_pga_field(double gain);

/* The output data rates the part has, in samples per second: 250 doubled up
 * to 16000 and nothing between [SBAS499C Table 13: f_MOD / 4096 down to
 * f_MOD / 64]. Returns CONFIG1's DR field for one - 6 for 250, 0 for 16000 -
 * or -1 for a rate the part does not have. [SBAS499?]: 250 is the only code
 * the bench has seen on this link; the others are the table's. */
int afe_rate_field(uint32_t rate_hz);

/* The register image for the given DR and PGA fields, internal reference, and
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
 * comment above LOFF_SENSP says so).
 *
 * referential: MISC1's SRB1 bit closes the switches that tie the SRB1 pin to
 * the inverting input of every channel, so each channel measures its own
 * electrode against the one on that pin [SBAS499C 10.2.2; no oracle in
 * ti-ads1299.c, which never writes MISC1]. The eight inverting inputs are then
 * ONE node, and two of the selections above change meaning with it:
 *
 *   LOFF_SENSN - one bit, AFE_REFERENCE_BIT. "When configured in a referential
 *                montage, only use one lead-off current source with the
 *                reference electrode" (same section): all eight would push
 *                eight times the excitation current through it.
 *   BIAS_SENSN - zero. A decision, not a datasheet fact: the part's text is
 *                silent, and summing the same node eight times into the
 *                average would make the bias follow the reference instead of
 *                the electrodes' common mode.
 */
void afe_registers(int rate_field, int pga_field, int test_signal, int bias_drive,
                   int lead_off, int referential, int shorted, int temperature,
                   struct afe_regs *r);

/*
 * shorted: every channel's inputs taken off its pins and both set to the
 * common-mode voltage (VREFP + VREFN) / 2 - MUX 001, "to test inherent device
 * noise" [SBAS499C 9.3.1.1.1, read 2026-10-12]. The test signal, asked for at
 * the same time, wins: both are positions of the same switch.
 *
 * temperature: the LAST channel's inputs on the temperature sensor - MUX 100 -
 * whatever the others are on. That channel then has no electrodes, so its bit
 * leaves all four BIAS_SENS/LOFF_SENS masks: it must neither feed the bias
 * amplifier's average nor carry a lead-off current into the sensor.
 *
 * The sensor's channel does NOT take the prescribed gain when that is 24.
 * Measured on the board, 2026-10-12, with the sensor on the last channel: at
 * gains 4, 6, 8 and 12 it read a steady 42 C; at 24 it read about 94 mV,
 * which the datasheet's line turns into -79 C. The sensor's 145 mV [SBAS499C
 * 9.3.1.1.3] saturates the amplifier at that gain, and the datasheet says so
 * only for the supply measurement beside it. So the channel's own gain is the
 * prescribed one capped at 12 (afe_sensor_gain): a gain the others' divides
 * by a whole number, which keeps every sample of the channel a whole number
 * of the record's steps. The frame carries input-referred nanovolts, so
 * nothing downstream sees the difference.
 *
 * The MUX code itself (100) is confirmed by that reading: 42 C on a board
 * that had been running, and no other position gives about 150 mV.
 */
#define AFE_TEMPERATURE_CHANNEL (AFE_CHANNELS - 1)

/* The gain the sensor's channel runs at, for a prescribed gain. */
double afe_sensor_gain(double gain);

/* In a referential montage, the one negative-side lead-off bit that is
 * monitored - and what it then means is "the reference electrode". */
#define AFE_REFERENCE_BIT 0x01u

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

/* The same number in integers, for the path that converts every sample, at
 * this board's reference: code * 4.5e9 / (gain * 2^23), rounded half away
 * from zero, saturated like the other. This core has no double-precision
 * unit, so afe_code_to_nv() costs it five operations in software per sample;
 * measured on the board, 2026-10-13, the loop spent 828 us per conversion and
 * could not keep 1000 a second.
 *
 * Not "about the same": compared with afe_code_to_nv() for every one of the
 * 2^24 codes at each of the seven gains, 0 differ - the record's grid is the
 * one the clinical file's converter checks, and it does not move.
 * gain is one of the part's seven, as a whole number. */
int32_t afe_code_to_nv_exact(int32_t code, uint32_t gain);

/* Whether the full scale at this gain, +-VREF / gain, fits the frame format's
 * int32 nanovolts (+-2.147 V). With the 4.5 V reference, gains 1 and 2 do not:
 * a sample near full scale would wrap to the opposite sign. */
int afe_gain_fits_frame(double gain, double vref_uv);

/* The status word of an RDATAC frame starts with the nibble 1100. Anything
 * else means the frame was read out of step with the converter. */
int afe_status_valid(const uint8_t *status);

#endif /* AFE_REGS_H */
