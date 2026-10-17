/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A converter of the ADS129x class, modelled at its SPI interface.
 *
 * Every transfer the driver makes reaches fake_transfer() with the bytes the
 * driver put on MOSI, and gets back the bytes the part would put on MISO. The
 * model knows the command set, the register file with its power-up values,
 * RDATAC/SDATAC (a part in continuous mode ignores register commands), the
 * status word that leads every RDATA, and what each input switch position
 * reads: the electrodes (a voltage the test sets per channel), the inputs
 * shorted, the test generator (DC, or a square wave whose half period is an
 * exact number of conversions), the temperature sensor.
 *
 * Where its numbers come from: the register addresses, reset values and field
 * layouts are the ones the bridge read from the real part on 2026-10-04
 * (BRINGUP_AFE.md section 4) and the ones the M33 firmware writes and reads
 * back on silicon every time it is configured. The generator's amplitude,
 * VREF/2400, was measured (BRINGUP_AFE.md section 4.3). What it does NOT
 * model: noise, settling, the analogue path between an electrode and the
 * code, and timing - a conversion happens when the test says so.
 */

#ifndef ADS1299_TEST_FAKE_H
#define ADS1299_TEST_FAKE_H

#include "kernel_shim.h"

#define FAKE_REGS	0x18
#define FAKE_CHANNELS	8
#define FAKE_VREF_UV	4500000.0
#define FAKE_MAX_CHIPS	8

struct fake_ads1299 {
	u8 reg[FAKE_REGS];
	bool rdatac;		/* continuous read mode: register commands ignored */
	bool standby;		/* STANDBY: nothing but WAKEUP is decoded */
	unsigned int ignored_in_standby;
	bool started;
	u64 conversions;	/* conversions the part has made since power-up */

	/*
	 * A daisy chain: chips devices behind one chip select and one DIN, so
	 * every register write reaches all of them and one register file
	 * stands for every device. 0 is a single device. RDATA carries each
	 * device's status word and samples in chain order.
	 */
	unsigned int chips;
	unsigned int chips_wired;	/* 0: all; fewer: the chain ends early */
	bool chain_swapped;		/* fault: devices 1 and 2 answer in reverse */

	/* What is on the inputs. */
	double input_uv[FAKE_CHANNELS];	/* MUX normal: the electrodes */
	double chain_input_uv[FAKE_MAX_CHIPS][FAKE_CHANNELS]; /* devices 2 on */
	s32 offset_codes;		/* added to every code: the part's offset */
	u8 detached_p, detached_n;	/* electrodes off, per input */
	bool bias_detached;		/* the bias electrode off */
	u8 gpio_in;			/* GPIO1-4 levels driven from outside, bits 0-3 */
	double temp_c;			/* the die, for the temperature sensor */
	double avdd_uv, avss_uv, dvdd_uv;	/* the supplies, for MUX = 011 */
	u8 gpio_forced;			/* fault: outputs pulled away from their level */
	u8 gpio_forced_level;		/* ... to these levels */

	/* Faults the tests inject. */
	bool id_wrong;			/* the part answers a foreign ID */
	double generator_error;		/* multiplies the test generator's amplitude */
	int stuck_reg;			/* -1: none */
	u8 stuck_mask;			/* bits of stuck_reg a write does not change */
	int fail_transfers;		/* the next n transfers fail at the bus */
	unsigned int out_of_step;	/* the next n RDATA replies open 0100, not 1100 */
	bool reset_ignored;		/* RESET does not take: state is what was left */

	/* What happened, for the tests to read. */
	unsigned int transfers;
	unsigned int rdata_reads;
	unsigned int reg_reads[FAKE_REGS];
	unsigned int reg_writes[FAKE_REGS];
	unsigned int ignored_in_rdatac;
};

extern struct fake_ads1299 fake;

/* Power-up: every register at its reset value, the part in RDATAC. */
void fake_power_up(void);
/* PWDN: the datasheet does not say what the registers keep, so this fake
 * takes the worse answer - all back to their power-up values. */
void fake_lose_power(void);
/* One conversion, as the part makes it on its own clock in continuous mode. */
void fake_convert(void);
/* The bytes on the bus. Returns 0, or -EIO for an injected bus failure. */
int fake_transfer(const u8 *tx, u8 *rx, unsigned int len);

/* What the part would put out for a channel right now, as a 24-bit code. */
s32 fake_code(unsigned int channel);
/* The same for a device of the chain, 0 being the first. */
s32 fake_chain_code(unsigned int chip, unsigned int channel);
/* The conversion rate CONFIG1 selects, in samples per second. */
unsigned int fake_rate(void);
/* The gain of a channel's PGA field. */
int fake_gain(unsigned int channel);

#endif /* ADS1299_TEST_FAKE_H */
