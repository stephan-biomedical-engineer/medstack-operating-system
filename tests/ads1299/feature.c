// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host tests of ti-ads1299.c as it stands on the branch going upstream,
 * feature/ads1299-mcp2210-driver, which main.c cannot compile: main.c tests
 * the board branch, and the two drivers differ in what they offer.
 *
 * The driver is taken from that branch by the Makefile (FEATURE_DRIVER) and
 * included here as it ships, against the same shim and the same fake converter
 * as main.c. What this file covers is what the board branch does not have: the
 * CLK pin as a clock output, a daisy chain refused rather than half read, the
 * status word checked at probe, and the test generator's every setting.
 *
 * Every check here is consistency, not correctness: the fake was written from
 * the same reading of the datasheet as the driver, and this bench has one
 * converter, so a clock output feeding another part has not been seen on
 * silicon. The chain's own tests went with the driver code to
 * wip/ads1299-daisy-chain; they are in this file's history.
 */

#include FEATURE_DRIVER

#include "check.h"
#include "fake_ads1299.h"
#include "shim_test.h"

struct check_counters counters;

enum { R_CONFIG1 = 0x01, R_CONFIG2, R_CONFIG3, R_CH1SET = 0x05, R_CONFIG4 = 0x17 };
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

static void test_chain_refused(void)
{
	section("cadeia: recusada no probe, em vez de lida pela metade");

	/* A real chain of two on the bus, declared in the firmware: the
	 * driver reads one device's share of RDATA, so accepting it would
	 * give eight channels and lose the other eight without a word. */
	fresh();
	shim_prop_daisy = 2;
	shim_ext_clk = &ext_clk;
	fake.chips = 2;
	CHECK_EQ(probe(0), -EOPNOTSUPP);
	CHECK(!shim_iio->registered);
	CHECK(strstr(shim_error_log, "daisy chain of 2 devices") != NULL);
	CHECK_EQ(fake.rdata_reads, 0);		/* refused before any conversion */

	/* A chain of one is a single device. */
	fresh();
	shim_prop_daisy = 1;
	CHECK_EQ(probe(0), 0);
	CHECK(shim_iio->registered);
	CHECK_EQ(shim_iio->num_channels, 9);
}

static void test_status_word(void)
{
	section("palavra de status: 1100b em toda leitura do autoteste");

	/* One reply out of step, the first conversion the self test reads -
	 * one it discards, which is still checked. */
	fresh();
	fake.out_of_step = 1;
	CHECK_EQ(probe(0), -EIO);
	CHECK(!shim_iio->registered);
	CHECK(strstr(shim_error_log, "status word opens with 0x4") != NULL);

	/* In step, the same part probes. */
	fresh();
	CHECK_EQ(probe(0), 0);
	CHECK(shim_iio->registered);
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

static void test_test_input_needs_generator(void)
{
	unsigned int c;
	bool all;

	section("entrada de teste: nunca com o gerador desligado");
	fresh();
	CHECK_EQ(probe(0), 0);
	if (!shim_iio || !shim_iio->registered)
		return;
	CHECK_EQ(fake.reg[R_CONFIG2], 0xc0);			/* generator off */

	/* The test input with nothing driving it: refused, MUX untouched. */
	CHECK_EQ(store("input_mux", "test_signal"), -EINVAL);
	for (all = true, c = 0; c < 8; c++)
		all = all && (fake.reg[R_CH1SET + c] & 0x07) == 0;
	CHECK(all);
	CHECK(!strcmp(show("input_mux"), "normal\n"));

	/* Generator on first, then the test input. */
	CHECK(store("test_signal", "1x_slow") > 0);
	CHECK(store("input_mux", "test_signal") > 0);
	for (all = true, c = 0; c < 8; c++)
		all = all && (fake.reg[R_CH1SET + c] & 0x07) == 5;
	CHECK(all);

	/* Switching the generator off under it: refused, CONFIG2 untouched;
	 * another generator setting is fine. */
	CHECK_EQ(store("test_signal", "off"), -EINVAL);
	CHECK_EQ(fake.reg[R_CONFIG2], 0xd0);
	CHECK(store("test_signal", "2x_fast") > 0);
	CHECK_EQ(fake.reg[R_CONFIG2], 0xd5);

	/* The electrodes back first, then the generator off. */
	CHECK(store("input_mux", "normal") > 0);
	CHECK(store("test_signal", "off") > 0);
	CHECK_EQ(fake.reg[R_CONFIG2], 0xc0);

	section("entrada de teste: input_mux também recusa com o buffer ligado");
	buffer_on();
	CHECK_EQ(store("input_mux", "shorted"), -EBUSY);
	for (all = true, c = 0; c < 8; c++)
		all = all && (fake.reg[R_CH1SET + c] & 0x07) == 0;
	CHECK(all);
	buffer_off();
	CHECK(store("input_mux", "shorted") > 0);
	CHECK(store("input_mux", "normal") > 0);
}

static void test_bias_measurement(void)
{
	unsigned int c;
	bool all;

	section("BIAS_MEAS segue o MUX: medir o BIASIN liga o bit, sair desliga");
	fresh();
	CHECK_EQ(probe(0), 0);
	if (!shim_iio || !shim_iio->registered)
		return;
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x10, 0);	/* off after probe */

	CHECK(store("input_mux", "bias_measurement") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x10, 0x10);
	for (all = true, c = 0; c < 8; c++)
		all = all && (fake.reg[R_CH1SET + c] & 0x07) == 2;
	CHECK(all);
	/* Only BIAS_MEAS moved: reference buffer on, reserved 11, bias off. */
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0 | 0x10);

	CHECK(store("input_mux", "shorted") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x10, 0);
	CHECK(store("input_mux", "bias_measurement") > 0);
	CHECK(store("input_mux", "normal") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0);

	/* The bias drive does not touch it, and it does not touch the bias. */
	CHECK(store("input_mux", "bias_measurement") > 0);
	CHECK(store("bias_drive", "derived") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0 | 0x10 | 0x08 | 0x04);
	CHECK(store("bias_drive", "off") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0 | 0x10);
	CHECK(store("input_mux", "normal") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0);
}

static void test_biasref(void)
{
	static struct regulator biasref = { .uv = 2500000 };

	section("BIASREF: gerado dentro sem biasref-supply, de fora com ele");
	fresh();
	CHECK_EQ(probe(0), 0);
	if (!shim_iio || !shim_iio->registered)
		return;
	CHECK(store("bias_drive", "derived") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x0c, 0x0c);	/* BIASREF_INT, PD_BIAS */
	CHECK(store("bias_drive", "reference") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x0c, 0x0c);
	CHECK(store("bias_drive", "off") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x0c, 0);

	fresh();
	biasref.enabled = 0;
	shim_biasref = &biasref;
	CHECK_EQ(probe(0), 0);
	CHECK_EQ(biasref.enabled, 1);			/* held for the device */
	CHECK(store("bias_drive", "derived") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x0c, 0x04);	/* PD_BIAS, BIASREF fed */
	CHECK(store("bias_drive", "reference") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x0c, 0x04);
	CHECK(store("bias_drive", "off") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x0c, 0);
}

static void test_bias_lead_off(void)
{
	section("lead-off do eletrodo de bias: BIAS_LOFF_SENS e BIAS_STAT");
	fresh();
	CHECK_EQ(probe(0), 0);
	if (!shim_iio || !shim_iio->registered)
		return;
	CHECK(!strcmp(show("bias_lead_off"), "off\n"));
	CHECK(!strcmp(show("bias_lead_off_status"), "disabled\n"));
	/* Detached but not sensed: the bit means nothing, so it says so. */
	fake.bias_detached = true;
	CHECK(!strcmp(show("bias_lead_off_status"), "disabled\n"));
	fake.bias_detached = false;

	CHECK(store("bias_lead_off", "on") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x02, 0x02);
	CHECK_EQ(fake.reg[R_CONFIG4] & 0x02, 0x02);	/* comparators on */
	CHECK(!strcmp(show("bias_lead_off"), "on\n"));
	CHECK(!strcmp(show("bias_lead_off_status"), "connected\n"));

	section("BIAS_STAT lido do conversor, não do cache (D3)");
	/* The electrode comes off after the last read: a cached CONFIG3 would
	 * go on saying "connected". */
	fake.bias_detached = true;
	CHECK(!strcmp(show("bias_lead_off_status"), "disconnected\n"));
	fake.bias_detached = false;
	CHECK(!strcmp(show("bias_lead_off_status"), "connected\n"));

	section("BIAS_LOFF_SENS: o resto do CONFIG3 intocado, e recusas");
	CHECK(store("bias_drive", "derived") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0 | 0x0c | 0x02);
	CHECK(store("input_mux", "bias_measurement") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0 | 0x10 | 0x0c | 0x02);
	CHECK(store("input_mux", "normal") > 0);
	CHECK(store("bias_drive", "off") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0 | 0x02);
	/* Comparators off by whatever came before: sensing turns them on. */
	CHECK(store("bias_lead_off", "off") > 0);
	CHECK_EQ(regmap_update_bits(priv()->regmap, ADS1299_REG_CONFIG4,
				    ADS1299_MASK_CONFIG4_PD_LOFF_COMP, 0), 0);
	CHECK_EQ(fake.reg[R_CONFIG4] & 0x02, 0);
	CHECK(store("bias_lead_off", "on") > 0);
	CHECK_EQ(fake.reg[R_CONFIG4] & 0x02, 0x02);

	CHECK_EQ(store("bias_lead_off", "maybe"), -EINVAL);
	buffer_on();
	CHECK_EQ(store("bias_lead_off", "off"), -EBUSY);
	CHECK_EQ(fake.reg[R_CONFIG3] & 0x02, 0x02);
	/* The status is read while running. */
	CHECK(!strcmp(show("bias_lead_off_status"), "connected\n"));
	buffer_off();

	CHECK(store("bias_lead_off", "off") > 0);
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0);
	fake.bias_detached = true;
	CHECK(!strcmp(show("bias_lead_off_status"), "disabled\n"));
	fake.bias_detached = false;
}

int main(void)
{
	printf("ti-ads1299 (branch do PR) - verificações de host\n");
	test_single_device();
	test_clock_output();
	test_chain_refused();
	test_status_word();
	test_test_signal();
	test_test_input_needs_generator();
	test_bias_measurement();
	test_biasref();
	test_bias_lead_off();

	printf("\nNão coberto por esta suíte:\n"
	       "  - silício: a bancada tem um conversor, então a saída de clock\n"
	       "    alimentando outro não foi vista em hardware\n");
	return report();
}
