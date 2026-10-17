// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host tests of ti-ads1299.c as it stands on the branch going upstream,
 * feature/ads1299-mcp2210-driver, which main.c cannot compile: main.c tests
 * the board branch, and the two drivers differ in what they offer.
 *
 * The driver is taken from that branch by the Makefile (FEATURE_DRIVER) and
 * included here as it ships, against the same shim and the same fake converter
 * as main.c. What this file covers is what the board branch does not have: the
 * CLK pin as a clock output, the daisy chain, and the test generator's every
 * setting.
 *
 * Every check here is consistency, not correctness: the fake was written from
 * the same reading of the datasheet as the driver, and this bench has one
 * converter, so neither a clock output feeding another part nor a chain of
 * devices has been seen on silicon.
 */

#include FEATURE_DRIVER

#include "check.h"
#include "fake_ads1299.h"
#include "shim_test.h"

struct check_counters counters;

enum { R_CONFIG1 = 0x01, R_CONFIG2, R_CONFIG3, R_CH1SET = 0x05 };
#define CONFIG1_DAISY_EN	0x40
#define CONFIG1_CLK_EN		0x20

static struct spi_device spi;
static struct clk ext_clk = { .rate = 2048000 };

static int probe(int irq)
{
	memset(&spi, 0, sizeof(spi));
	spi.dev.name = "spi0.0";
	spi.irq = irq;
	return shim_spi_driver->probe(&spi);
}

/* Probe with a fresh shim, after the test has set the firmware properties. */
static void fresh(void)
{
	shim_reset();
	fake_power_up();
}

static struct ads1299_private *priv(void)
{
	return iio_priv(shim_iio);
}

static struct device_attribute *attr(const char *name)
{
	for (struct attribute **a = ads1299_attributes; *a; a++)
		if (!strcmp((*a)->name, name))
			return container_of(*a, struct device_attribute, attr);
	return NULL;
}

static const char *show(const char *name)
{
	static char buf[4096];
	struct device_attribute *a = attr(name);
	ssize_t n;

	if (!a || !a->show)
		return "<no attribute>";
	n = a->show(&shim_iio->dev, a, buf);
	if (n < 0)
		snprintf(buf, sizeof(buf), "<error %zd>", n);
	return buf;
}

static ssize_t store(const char *name, const char *value)
{
	struct device_attribute *a = attr(name);

	if (!a || !a->store)
		return -ENOENT;
	return a->store(&shim_iio->dev, a, value, strlen(value));
}

static void buffer_on_mask(unsigned long mask)
{
	shim_iio->scan_mask_storage[0] = mask;
	shim_iio->active_scan_mask = shim_iio->scan_mask_storage;
	shim_iio->masklength = (unsigned int)shim_iio->num_channels;
	(void)shim_iio->info->update_scan_mode(shim_iio, shim_iio->active_scan_mask);
	shim_iio->buffer_enabled = true;
	(void)shim_iio->setup_ops->postenable(shim_iio);
}

static void buffer_on(void)
{
	buffer_on_mask((1UL << shim_iio->num_channels) - 1);
}

static void buffer_off(void)
{
	(void)shim_iio->setup_ops->predisable(shim_iio);
	shim_iio->buffer_enabled = false;
}

static void tick(void)
{
	priv()->timer.function(&priv()->timer);
}

static s32 code24(u32 v)
{
	return sign_extend32(v & 0xffffff, 23);
}

static int read_raw_chan(int index, long mask, int *val, int *val2)
{
	return shim_iio->info->read_raw(shim_iio, &shim_iio->channels[index],
					val, val2, mask);
}

/* ------------------------------------------------------------------ tests */

static void test_single_device(void)
{
	section("um conversor: DAISY_EN e CLK_EN escritos, sem saída de clock");
	fresh();
	CHECK_EQ(probe(0), 0);
	if (!shim_iio)
		return;
	CHECK(shim_iio->registered);
	CHECK_EQ(shim_iio->num_channels, 9);		/* eight voltages, timestamp */
	CHECK_EQ(priv()->num_chips, 1);
	/* 0x96 out of reset: DAISY_EN and CLK_EN were already 0, so the write
	 * the driver asks for changes nothing - and the bits stay 0. */
	CHECK_EQ(fake.reg[R_CONFIG1] & (CONFIG1_DAISY_EN | CONFIG1_CLK_EN), 0);
	CHECK_EQ(priv()->rdata_xfer.len, 28);		/* RDATA, status, 8 x 24 bits */
	CHECK_EQ(shim_iio->channels[0].address, 4);
	CHECK_EQ(shim_iio->channels[7].address, 25);
	CHECK(shim_clk_hw == NULL);			/* no #clock-cells */

	/* A part left with both bits set, on which RESET did not take: the
	 * driver writes them rather than trusting the reset. */
	fresh();
	fake.reg[R_CONFIG1] |= CONFIG1_DAISY_EN | CONFIG1_CLK_EN;
	fake.reset_ignored = true;
	CHECK_EQ(probe(0), 0);
	CHECK_EQ(fake.reg[R_CONFIG1] & (CONFIG1_DAISY_EN | CONFIG1_CLK_EN), 0);
}

static void test_clock_output(void)
{
	unsigned int writes;
	int val;

	section("CLK como saída de clock: #clock-cells, prepare liga CLK_EN");
	fresh();
	shim_prop_clock_cells = true;
	CHECK_EQ(probe(0), 0);
	CHECK(shim_clk_hw != NULL);
	if (!shim_clk_hw)
		return;
	CHECK(!strcmp(shim_clk_hw_name, "spi0.0-clk"));
	CHECK_EQ(shim_clk_hw_ops->recalc_rate(shim_clk_hw, 0), 2048000);
	CHECK_EQ(fake.reg[R_CONFIG1] & CONFIG1_CLK_EN, 0);	/* CLK at 3-state */
	CHECK_EQ(shim_clk_hw_ops->is_prepared(shim_clk_hw), 0);

	CHECK_EQ(shim_clk_hw_ops->prepare(shim_clk_hw), 0);
	CHECK_EQ(fake.reg[R_CONFIG1] & CONFIG1_CLK_EN, CONFIG1_CLK_EN);
	CHECK_EQ(shim_clk_hw_ops->is_prepared(shim_clk_hw), 1);
	/* Only CLK_EN moved: the reserved bits and the rate are where they were. */
	CHECK_EQ(fake.reg[R_CONFIG1], 0x96 | CONFIG1_CLK_EN);

	/* A rate change keeps the clock output running. */
	CHECK_EQ(shim_iio->info->write_raw(shim_iio, &shim_iio->channels[0], 500, 0,
					   IIO_CHAN_INFO_SAMP_FREQ), 0);
	CHECK_EQ(fake.reg[R_CONFIG1] & CONFIG1_CLK_EN, CONFIG1_CLK_EN);
	CHECK_EQ(fake.reg[R_CONFIG1] & 0x07, 0x05);	/* 16000 >> 5 */
	CHECK_EQ(read_raw_chan(0, IIO_CHAN_INFO_SAMP_FREQ, &val, NULL), IIO_VAL_INT);
	CHECK_EQ(val, 500);

	shim_clk_hw_ops->unprepare(shim_clk_hw);
	CHECK_EQ(fake.reg[R_CONFIG1] & CONFIG1_CLK_EN, 0);
	CHECK_EQ(fake.reg[R_CONFIG1] & 0x07, 0x05);
	CHECK_EQ(shim_clk_hw_ops->is_prepared(shim_clk_hw), 0);

	/* is_prepared answers from the cache: no bus traffic to ask. */
	writes = fake.transfers;
	(void)shim_clk_hw_ops->is_prepared(shim_clk_hw);
	CHECK_EQ(fake.transfers, writes);

	section("CLK como saída de clock: nome dado e recusas");
	fresh();
	shim_prop_clock_cells = true;
	shim_prop_clock_name = "afe_clk";
	CHECK_EQ(probe(0), 0);
	CHECK(!strcmp(shim_clk_hw_name, "afe_clk"));

	/* An external clock means CLKSEL is low and CLK is an input. */
	fresh();
	shim_prop_clock_cells = true;
	shim_ext_clk = &ext_clk;
	CHECK_EQ(probe(0), -EINVAL);
	CHECK(shim_clk_hw == NULL);
	CHECK(strstr(shim_last_error, "#clock-cells") != NULL);

	/* A part that fails its self test is not a clock source for others. */
	fresh();
	shim_prop_clock_cells = true;
	fake.generator_error = 0.5;
	CHECK(probe(0) != 0);
	CHECK(shim_clk_hw == NULL);
}

static void test_daisy_chain(void)
{
	unsigned int c;
	int val, val2;

	section("cadeia de 2: 16 canais, um quadro de 2 x 27 bytes");
	fresh();
	shim_prop_daisy = 2;
	shim_ext_clk = &ext_clk;
	fake.chips = 2;
	CHECK_EQ(probe(0), 0);
	if (!shim_iio || !shim_iio->registered)
		return;
	CHECK_EQ(priv()->num_chips, 2);
	CHECK_EQ(shim_iio->num_channels, 17);		/* 16 voltages, timestamp */
	CHECK_EQ(priv()->rdata_xfer.len, 1 + 2 * 27);
	CHECK_EQ(shim_iio->channels[8].address, 1 + 27 + 3);	/* device 2, ch 1 */
	CHECK_EQ(shim_iio->channels[15].address, 1 + 27 + 3 + 21);
	CHECK_EQ(shim_iio->channels[16].type, IIO_TIMESTAMP);
	CHECK_EQ(shim_iio->channels[16].scan_index, 16);
	CHECK_EQ(fake.reg[R_CONFIG1] & CONFIG1_DAISY_EN, 0);	/* daisy-chain mode */

	/* Distinct inputs on every channel of both devices. */
	for (c = 0; c < 8; c++) {
		fake.input_uv[c] = 1000.0 * (c + 1);
		fake.chain_input_uv[1][c] = -1000.0 * (c + 1);
	}

	section("cadeia de 2: o buffer demultiplexa cada dispositivo no seu canal");
	buffer_on();
	fake_convert();
	tick();
	shim_spi_complete_one();
	buffer_off();
	CHECK_EQ(shim_scan_count, 1);
	CHECK_EQ(shim_scans[0].count, 16);
	{
		bool exact = true;

		for (c = 0; c < 8; c++) {
			exact = exact && code24(shim_scans[0].value[c]) == fake_chain_code(0, c);
			exact = exact && code24(shim_scans[0].value[8 + c]) == fake_chain_code(1, c);
		}
		CHECK(exact);
		CHECK(code24(shim_scans[0].value[0]) > 0);
		CHECK(code24(shim_scans[0].value[8]) < 0);
	}

	section("cadeia de 2: leitura direta e escala de um canal do dispositivo 2");
	CHECK_EQ(read_raw_chan(9, IIO_CHAN_INFO_RAW, &val, &val2), IIO_VAL_INT);
	CHECK_EQ(val, fake_chain_code(1, 1));
	CHECK_EQ(read_raw_chan(9, IIO_CHAN_INFO_SCALE, &val, &val2), IIO_VAL_INT_PLUS_NANO);
	CHECK_EQ(val2, 22351);			/* 4.5 V / (24 * 2^23), in mV x 1e9 */

	section("cadeia de 2: um registro vale para os dois dispositivos");
	/* Only device 2's channel 1 in the scan: CH1SET powered, the rest down. */
	buffer_on_mask(BIT(8));
	CHECK_EQ(fake.reg[R_CH1SET] & 0x80, 0);
	for (c = 1; c < 8; c++)
		CHECK_EQ(fake.reg[R_CH1SET + c] & 0x80, 0x80);
	fake_convert();
	tick();
	shim_spi_complete_one();
	buffer_off();
	CHECK_EQ(shim_scans[shim_scan_count - 1].count, 1);
	CHECK_EQ(code24(shim_scans[shim_scan_count - 1].value[0]), fake_chain_code(1, 0));
	/* One gain for both devices. */
	CHECK_EQ(shim_iio->info->write_raw(shim_iio, &shim_iio->channels[0], 4, 0,
					   IIO_CHAN_INFO_HARDWAREGAIN), 0);
	CHECK_EQ(read_raw_chan(15, IIO_CHAN_INFO_SCALE, &val, &val2), IIO_VAL_INT_PLUS_NANO);
	CHECK_EQ(val2, 134110);			/* 4.5 V / (4 * 2^23) */

	section("cadeia de 2: o que uma cadeia não pode fazer é recusado");
	CHECK_EQ(store("bias_drive", "derived"), -EINVAL);
	CHECK_EQ(store("bias_drive", "reference"), -EINVAL);
	CHECK(store("bias_drive", "off") > 0);
	CHECK(!strcmp(show("bias_drive_available"), "off\n"));
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x04, 0);		/* PD_BIAS: amplifier down */
	CHECK(!strcmp(show("lead_off_status"), "<error -95>"));	/* -EOPNOTSUPP */
}

static void test_daisy_refusals(void)
{
	section("cadeia: topologia recusada no probe");

	/* Shared DIN: no device can be told to drive the others' clock. */
	fresh();
	shim_prop_daisy = 2;
	fake.chips = 2;
	CHECK_EQ(probe(0), -EINVAL);
	CHECK(strstr(shim_last_error, "external clock") != NULL);

	fresh();
	shim_prop_daisy = 9;
	shim_ext_clk = &ext_clk;
	CHECK_EQ(probe(0), -EINVAL);

	/* A chain shorter than declared: device 2's share reads zeros, its
	 * status word does not open with 1100, the self test says which. */
	fresh();
	shim_prop_daisy = 2;
	shim_ext_clk = &ext_clk;
	fake.chips = 2;
	fake.chips_wired = 1;
	CHECK_EQ(probe(0), -EIO);
	CHECK(strstr(shim_error_log, "device 2 of 2") != NULL);

	/* A chain of one is a single device. */
	fresh();
	shim_prop_daisy = 1;
	CHECK_EQ(probe(0), 0);
	CHECK_EQ(shim_iio->num_channels, 9);
	CHECK(strstr(show("bias_drive_available"), "derived") != NULL);
}

static void test_daisy_order(void)
{
	section("cadeia de 3: cada dispositivo no seu lugar, em ordem");
	fresh();
	shim_prop_daisy = 3;
	shim_ext_clk = &ext_clk;
	fake.chips = 3;
	CHECK_EQ(probe(0), 0);
	if (!shim_iio || !shim_iio->registered)
		return;
	CHECK_EQ(shim_iio->num_channels, 25);
	CHECK_EQ(priv()->rdata_xfer.len, 1 + 3 * 27);
	for (unsigned int c = 0; c < 8; c++) {
		fake.input_uv[c] = 100.0;
		fake.chain_input_uv[1][c] = 200.0;
		fake.chain_input_uv[2][c] = 300.0;
	}
	buffer_on();
	fake_convert();
	tick();
	shim_spi_complete_one();
	buffer_off();
	CHECK_EQ(shim_scans[0].count, 24);
	CHECK_EQ(code24(shim_scans[0].value[7]), fake_chain_code(0, 7));
	CHECK_EQ(code24(shim_scans[0].value[8]), fake_chain_code(1, 0));
	CHECK_EQ(code24(shim_scans[0].value[23]), fake_chain_code(2, 7));
	CHECK(code24(shim_scans[0].value[8]) != code24(shim_scans[0].value[0]));
	CHECK(code24(shim_scans[0].value[16]) != code24(shim_scans[0].value[8]));
}

static void test_test_signal(void)
{
	/* Every name, and the CONFIG2 byte it must write [SBAS499 Table 14]:
	 * 110 reserved, INT_CAL, 0 reserved, CAL_AMP, CAL_FREQ. */
	static const struct { const char *name; u8 config2; } table[] = {
		{ "off",     0xc0 },
		{ "1x_slow", 0xd0 },
		{ "1x_fast", 0xd1 },
		{ "1x_dc",   0xd3 },
		{ "2x_slow", 0xd4 },
		{ "2x_fast", 0xd5 },
		{ "2x_dc",   0xd7 },
	};
	char name[16];
	int val, val2;

	section("sinal de teste: os sete valores, o byte do CONFIG2 e a volta");
	fresh();
	CHECK_EQ(probe(0), 0);
	if (!shim_iio || !shim_iio->registered)
		return;
	CHECK(!strcmp(show("test_signal_available"),
		      "off 1x_slow 1x_fast 1x_dc 2x_slow 2x_fast 2x_dc\n"));
	for (unsigned int i = 0; i < ARRAY_SIZE(table); i++) {
		snprintf(name, sizeof(name), "%s\n", table[i].name);
		CHECK(store("test_signal", name) > 0);
		CHECK_EQ(fake.reg[R_CONFIG2], table[i].config2);
		CHECK(!strcmp(show("test_signal"), name));
	}

	section("sinal de teste: em DC, a leitura é o nível do gerador");
	CHECK(store("input_mux", "test_signal") > 0);
	CHECK(store("test_signal", "1x_dc") > 0);
	CHECK_EQ(read_raw_chan(0, IIO_CHAN_INFO_RAW, &val, &val2), IIO_VAL_INT);
	CHECK_EQ(val, fake_code(0));
	CHECK(val > 0);
	CHECK(store("test_signal", "2x_dc") > 0);
	CHECK_EQ(read_raw_chan(0, IIO_CHAN_INFO_RAW, &val2, &val2), IIO_VAL_INT);
	CHECK_EQ(val2, fake_code(0));
	CHECK(val2 > val + val / 2);			/* twice, give or take rounding */
	CHECK(store("input_mux", "normal") > 0);

	section("sinal de teste: recusas");
	CHECK_EQ(store("test_signal", "loud"), -EINVAL);
	CHECK_EQ(store("test_signal", "1x"), -EINVAL);
	CHECK_EQ(store("test_signal", "1x_dc_"), -EINVAL);
	CHECK_EQ(fake.reg[R_CONFIG2], 0xd7);		/* nothing written */

	/* With the buffer running, a test channel's samples would change. */
	buffer_on();
	CHECK_EQ(store("test_signal", "off"), -EBUSY);
	CHECK_EQ(fake.reg[R_CONFIG2], 0xd7);
	buffer_off();
	CHECK(store("test_signal", "off") > 0);
	CHECK_EQ(fake.reg[R_CONFIG2], 0xc0);

	/* CAL_FREQ = 10 is "do not use"; only a register write puts it there,
	 * and show says it has no name rather than calling it something. */
	CHECK_EQ(regmap_write(priv()->regmap, ADS1299_REG_CONFIG2, 0xd2), 0);
	CHECK(!strcmp(show("test_signal"), "<error -22>"));
	/* The generator off: amplitude and frequency bits do not matter. */
	CHECK_EQ(regmap_write(priv()->regmap, ADS1299_REG_CONFIG2, 0xc7), 0);
	CHECK(!strcmp(show("test_signal"), "off\n"));
}

int main(void)
{
	printf("ti-ads1299 (branch do PR) - verificações de host\n");
	test_single_device();
	test_clock_output();
	test_daisy_chain();
	test_daisy_refusals();
	test_daisy_order();
	test_test_signal();

	printf("\nNão coberto por esta suíte:\n"
	       "  - silício: a bancada tem um conversor, então nem a saída de clock\n"
	       "    alimentando outro nem uma cadeia foram vistas em hardware\n"
	       "  - o que um RREG devolve numa cadeia: o datasheet não diz\n");
	return report();
}
