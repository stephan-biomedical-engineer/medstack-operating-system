// SPDX-License-Identifier: GPL-2.0-only
/* The fake converter - see fake_ads1299.h. */

#include <math.h>

#include "fake_ads1299.h"

struct fake_ads1299 fake;

enum {
	R_ID = 0x00, R_CONFIG1, R_CONFIG2, R_CONFIG3, R_LOFF, R_CH1SET,
	R_BIAS_SENSP = 0x0d, R_BIAS_SENSN, R_LOFF_SENSP, R_LOFF_SENSN, R_LOFF_FLIP,
	R_LOFF_STATP, R_LOFF_STATN, R_GPIO, R_MISC1, R_MISC2, R_CONFIG4,
};

static const int pga_gain[8] = { 1, 2, 4, 6, 8, 12, 24, 0 };

void fake_power_up(void)
{
	memset(&fake, 0, sizeof(fake));
	/* Table 11, as read from the part by the bridge on 2026-10-04. */
	fake.reg[R_ID] = 0x3e;
	fake.reg[R_CONFIG1] = 0x96;
	fake.reg[R_CONFIG2] = 0xc0;
	fake.reg[R_CONFIG3] = 0x60;
	for (int c = 0; c < FAKE_CHANNELS; c++)
		fake.reg[R_CH1SET + c] = 0x61;	/* gain 24, inputs shorted */
	fake.reg[R_GPIO] = 0x0f;
	fake.rdatac = true;
	fake.stuck_reg = -1;
	fake.generator_error = 1.0;
}

unsigned int fake_rate(void)
{
	return 2048000u >> (7 + (fake.reg[R_CONFIG1] & 0x07));
}

int fake_gain(unsigned int channel)
{
	return pga_gain[(fake.reg[R_CH1SET + channel] >> 4) & 0x07];
}

static double generator_uv(void)
{
	const u8 cfg2 = fake.reg[R_CONFIG2];
	double amp;
	u64 half;

	if (!(cfg2 & 0x10))		/* INT_CAL: the generator is not routed */
		return 0.0;
	amp = FAKE_VREF_UV / 2400.0 * ((cfg2 & 0x04) ? 2.0 : 1.0) * fake.generator_error;
	switch (cfg2 & 0x03) {
	case 3:				/* held at DC */
		return amp;
	case 1:				/* f_CLK / 2^20 */
		half = (u64)fake_rate() * (1u << 19) / 2048000u;
		break;
	default:			/* f_CLK / 2^21 */
		half = (u64)fake_rate() * (1u << 20) / 2048000u;
		break;
	}
	return ((fake.conversions / half) % 2 == 0) ? amp : -amp;
}

s32 fake_code(unsigned int channel)
{
	const u8 chset = fake.reg[R_CH1SET + channel];
	const int gain = fake_gain(channel);
	double uv;
	double code;

	if (chset & 0x80)		/* powered down */
		return 0;
	switch (chset & 0x07) {
	case 0:
		uv = fake.input_uv[channel];
		break;
	case 4:				/* the temperature sensor, at 25 degC */
		uv = 145300.0;
		break;
	case 5:
		uv = generator_uv();
		break;
	default:			/* shorted and the supply/bias positions */
		uv = 0.0;
		break;
	}
	code = round(uv * gain * 8388608.0 / FAKE_VREF_UV) + fake.offset_codes;
	if (code > 8388607.0)
		code = 8388607.0;
	if (code < -8388608.0)
		code = -8388608.0;
	return (s32)code;
}

static void latch_lead_off(void)
{
	/* With the comparators powered every status bit follows its input,
	 * sensed or not. The datasheet only says a bit is valid where its input
	 * is sensed; what the others read is not specified, and the hostile
	 * answer - the same as a sensed one - is the one that tests whether the
	 * driver throws them away. */
	const bool powered = fake.reg[R_CONFIG4] & 0x02;

	fake.reg[R_LOFF_STATP] = powered ? fake.detached_p : 0;
	fake.reg[R_LOFF_STATN] = powered ? fake.detached_n : 0;
}

void fake_convert(void)
{
	fake.conversions++;
	latch_lead_off();
}

static void put_be24(u8 *p, s32 v)
{
	const u32 u = (u32)v & 0xffffff;

	p[0] = (u8)(u >> 16);
	p[1] = (u8)(u >> 8);
	p[2] = (u8)u;
}

static void rdata(u8 *rx, unsigned int len)
{
	const u8 sp = fake.reg[R_LOFF_STATP];
	const u8 sn = fake.reg[R_LOFF_STATN];

	u8 sync = 0xc0;

	fake.rdata_reads++;
	if (len < 4)
		return;
	/* A reply a bit out of step: the opening nibble is the first thing off. */
	if (fake.out_of_step) {
		fake.out_of_step--;
		sync = 0x40;
	}
	/* 1100, LOFF_STATP, LOFF_STATN, GPIO[7:4] */
	rx[1] = (u8)(sync | (sp >> 4));
	rx[2] = (u8)((sp << 4) | (sn >> 4));
	rx[3] = (u8)((sn << 4) | (fake.reg[R_GPIO] >> 4));
	for (unsigned int c = 0; c < FAKE_CHANNELS && 4 + 3 * c + 2 < len; c++)
		put_be24(rx + 4 + 3 * c, fake_code(c));
}

static void write_reg(unsigned int r, u8 v)
{
	if (r >= FAKE_REGS)
		return;
	fake.reg_writes[r]++;
	if (r == R_ID || r == R_LOFF_STATP || r == R_LOFF_STATN)
		return;			/* read only */
	if ((int)r == fake.stuck_reg)
		v = (u8)((v & ~fake.stuck_mask) | (fake.reg[r] & fake.stuck_mask));
	fake.reg[r] = v;
}

int fake_transfer(const u8 *tx, u8 *rx, unsigned int len)
{
	u8 op;

	fake.transfers++;
	if (fake.fail_transfers > 0) {
		fake.fail_transfers--;
		return -EIO;
	}
	if (!len || !tx)
		return 0;
	op = tx[0];
	if (rx && rx != tx)
		memset(rx, 0, len);

	if ((op & 0xe0) == 0x20 || (op & 0xe0) == 0x40) {
		const unsigned int first = op & 0x1f;
		const unsigned int n = len > 1 ? (unsigned int)tx[1] + 1 : 0;

		if (fake.rdatac) {
			/* A part in continuous mode does not decode register
			 * commands; it answers zeros. */
			fake.ignored_in_rdatac++;
			if (rx)
				memset(rx, 0, len);
			return 0;
		}
		for (unsigned int i = 0; i < n && 2 + i < len; i++) {
			if ((op & 0xe0) == 0x40) {
				write_reg(first + i, tx[2 + i]);
			} else if (first + i < FAKE_REGS) {
				fake.reg_reads[first + i]++;
				if (rx)
					rx[2 + i] = fake.id_wrong && first + i == R_ID ? 0x00
										: fake.reg[first + i];
			}
		}
		return 0;
	}

	switch (op) {
	case 0x02:			/* WAKEUP */
		break;
	case 0x06:			/* RESET */
		{
			bool wrong = fake.id_wrong;
			int stuck = fake.stuck_reg;
			u8 mask = fake.stuck_mask;
			double gen = fake.generator_error;
			double in[FAKE_CHANNELS];
			s32 off = fake.offset_codes;
			u8 dp = fake.detached_p, dn = fake.detached_n;

			memcpy(in, fake.input_uv, sizeof(in));
			fake_power_up();
			fake.id_wrong = wrong;
			fake.stuck_reg = stuck;
			fake.stuck_mask = mask;
			fake.generator_error = gen;
			memcpy(fake.input_uv, in, sizeof(in));
			fake.offset_codes = off;
			fake.detached_p = dp;
			fake.detached_n = dn;
		}
		break;
	case 0x08:			/* START */
		fake.started = true;
		/* In single-shot mode START makes exactly one conversion, and
		 * restarts the generator's divider with it. */
		if (fake.reg[R_CONFIG4] & 0x08)
			fake_convert();
		break;
	case 0x0a:			/* STOP */
		fake.started = false;
		break;
	case 0x10:			/* RDATAC */
		fake.rdatac = true;
		break;
	case 0x11:			/* SDATAC */
		fake.rdatac = false;
		break;
	case 0x12:			/* RDATA */
		if (rx)
			rdata(rx, len);
		break;
	default:
		break;
	}
	return 0;
}
