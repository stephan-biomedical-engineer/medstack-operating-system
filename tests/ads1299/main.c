// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host tests of the converter's kernel driver, ti-ads1299.c
 * (docs/implementation_plan_usb_parity.md, phase 0.a).
 *
 * The driver is included here as it ships - no test macro, no #ifdef - and
 * runs against kernel_shim.c and fake_ads1299.c, which sit below it at the
 * SPI core. Three kinds of outcome, as in tests/mcp2210:
 *
 *   CHECK   a behaviour the driver has and must keep;
 *   DEFECT  a behaviour the plan says is wrong today, counted apart: it does
 *           not fail the suite, and when it stops being true the runner says
 *           "resolved - promote this to a CHECK", which is how a phase of the
 *           plan proves it landed;
 *   oracle  where the M33 firmware programs the same part, its register
 *           builder (afe_regs.c, which has been read back on silicon at every
 *           configuration since 2026-10-08) is compiled in and the two
 *           drivers are held to the same bytes.
 */

#include "../../linux-med/drivers/iio/adc/ti-ads1299.c"

#include "check.h"
#include "fake_ads1299.h"
#include "shim_test.h"
#include "afe_regs.h"

struct check_counters counters;

enum {
	R_CONFIG1 = 0x01, R_CONFIG2, R_CONFIG3, R_CH1SET = 0x05,
	R_BIAS_SENSP = 0x0d, R_BIAS_SENSN, R_LOFF_SENSP, R_LOFF_SENSN,
	R_MISC1 = 0x15, R_CONFIG4 = 0x17,
};

static struct spi_device spi;

static int probe(int irq)
{
	shim_reset();
	memset(&spi, 0, sizeof(spi));
	spi.dev.name = "spi0.0";
	spi.irq = irq;
	return shim_spi_driver->probe(&spi);
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

static void buffer_on(void)
{
	shim_iio->scan_mask_storage[0] = (1UL << shim_iio->num_channels) - 1;
	shim_iio->active_scan_mask = shim_iio->scan_mask_storage;
	shim_iio->masklength = (unsigned int)shim_iio->num_channels;
	(void)shim_iio->info->update_scan_mode(shim_iio, shim_iio->active_scan_mask);
	shim_iio->buffer_enabled = true;
	(void)shim_iio->setup_ops->postenable(shim_iio);
}

static void buffer_off(void)
{
	(void)shim_iio->setup_ops->predisable(shim_iio);
	shim_iio->buffer_enabled = false;
}

/* The host timer the usb link samples on: one tick, one sample due. */
static void tick(void)
{
	priv()->timer.function(&priv()->timer);
}

static s32 code24(u32 v)
{
	return sign_extend32(v & 0xffffff, 23);
}

static int write_raw(long mask, int val)
{
	return shim_iio->info->write_raw(shim_iio, &shim_iio->channels[0], val, 0, mask);
}

static int read_raw(long mask, int *val, int *val2)
{
	return shim_iio->info->read_raw(shim_iio, &shim_iio->channels[0], val, val2, mask);
}

/* ------------------------------------------------------------------ tests */

static void test_probe(void)
{
	int ret;

	section("probe: o driver encontra, configura e testa o conversor");
	fake_power_up();
	ret = probe(0);
	CHECK_EQ(ret, 0);
	CHECK(shim_iio && shim_iio->registered);
	if (!shim_iio || ret)
		return;
	CHECK(!strcmp(shim_iio->name, "ads1299-8"));
	CHECK_EQ(shim_iio->num_channels, 9);	/* eight voltages and a timestamp */
	/* Out of RDATAC before any register: a part in continuous mode ignores
	 * register commands, and the ID read would see zeros. */
	CHECK(!fake.rdatac);
	CHECK_EQ(fake.ignored_in_rdatac, 0);
	CHECK_EQ(fake.reg[R_CONFIG2], 0xc0);	/* generator off */
	CHECK_EQ(fake.reg[R_CONFIG3], 0xe0);	/* reference buffer on, bias off */
	CHECK_EQ(fake.reg[R_MISC1], 0x00);	/* bipolar */
	CHECK_EQ(fake.reg[R_BIAS_SENSP], 0x00);
	CHECK_EQ(fake.reg[R_BIAS_SENSN], 0x00);
	for (int c = 0; c < 8; c++)
		CHECK_EQ(fake.reg[R_CH1SET + c] & 0x77, 0x60);	/* gain 24, electrodes */
	CHECK(fake.reg[R_CONFIG4] & 0x02);	/* lead-off comparators powered */
	/* The self test ran: two phases of 12 single-shot reads. */
	CHECK(fake.rdata_reads >= 24);
	CHECK(!strcmp(show("timestamp_source"), "host_timer\n"));

	DEFECT(fake.reg[R_LOFF_SENSP] == 0 && fake.reg[R_LOFF_SENSN] == 0,
	       "comparadores de lead-off ligados e nenhum canal habilitado: um registro "
	       "que diz 'monitorado' sem nada monitorado",
	       "usb_parity, fase 2");
	CHECK(!strcmp(show("lead_off_status"), "disabled\n"));
}

static void test_probe_refusals(void)
{
	int ret;

	section("probe: o que ele recusa");

	fake_power_up();
	fake.id_wrong = true;
	ret = probe(0);
	CHECK_EQ(ret, -ENODEV);
	CHECK(!shim_iio->registered);

	/* The generator half as strong as it should be: a dead PGA stage, a
	 * wrong reference. Outside +-20%, the part is never registered. */
	fake_power_up();
	fake.generator_error = 0.5;
	ret = probe(0);
	CHECK_EQ(ret, -EIO);
	CHECK(!shim_iio->registered);
	CHECK_MSG(strstr(shim_last_error, "self test") != NULL, shim_last_error);

	/* Inside the tolerance it is still the part. */
	fake_power_up();
	fake.generator_error = 1.15;
	CHECK_EQ(probe(0), 0);

	/* A bus that fails on the first transfer: nothing is registered. */
	fake_power_up();
	fake.fail_transfers = 1;
	ret = probe(0);
	CHECK(ret != 0);
	CHECK(!shim_iio->registered);
}

static void test_against_m33(void)
{
	static const int rates[] = { 250, 500, 1000, 2000, 4000, 8000, 16000 };
	struct afe_regs r;

	section("o mesmo conversor, os mesmos bytes que o firmware do M33 escreve");
	fake_power_up();
	if (probe(0))
		return;

	for (int ref = 0; ref < 2; ref++) {
		for (int bias = 0; bias < 2; bias++) {
			for (int order = 0; order < 2; order++) {
				const char *mont = ref ? "referential" : "bipolar";
				const char *drive = bias ? "derived" : "off";
				char what[96];

				if (order) {
					CHECK(store("bias_drive", drive) > 0);
					CHECK(store("montage", mont) > 0);
				} else {
					CHECK(store("montage", mont) > 0);
					CHECK(store("bias_drive", drive) > 0);
				}
				afe_registers(6, 6, 0, bias, 0, ref, 0, 0, &r);
				snprintf(what, sizeof(what), "%s, bias %s, %s", mont, drive,
					 order ? "bias primeiro" : "montagem primeiro");
				CHECK_MSG(fake.reg[R_BIAS_SENSP] == r.sens[0], what);
				CHECK_MSG(fake.reg[R_BIAS_SENSN] == r.sens[1], what);
				CHECK_MSG(fake.reg[R_MISC1] == r.misc1, what);
				CHECK_MSG((fake.reg[R_CONFIG3] & 0x8c) == (r.config3 & 0x8c), what);
			}
		}
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(rates); i++) {
		CHECK_EQ(write_raw(IIO_CHAN_INFO_SAMP_FREQ, rates[i]), 0);
		afe_registers(afe_rate_field((uint32_t)rates[i]), 6, 0, 0, 0, 0, 0, 0, &r);
		CHECK_EQ(fake.reg[R_CONFIG1], r.config1);
	}
	CHECK_EQ(write_raw(IIO_CHAN_INFO_SAMP_FREQ, 300), -EINVAL);
	CHECK_EQ(fake.reg[R_CONFIG1], r.config1);	/* unchanged by a refusal */
	CHECK_EQ(write_raw(IIO_CHAN_INFO_SAMP_FREQ, 250), 0);

	for (int field = 0; field < 7; field++) {
		static const int gains[] = { 1, 2, 4, 6, 8, 12, 24 };
		bool same = true;
		int val = 0, val2 = 0;

		CHECK_EQ(write_raw(IIO_CHAN_INFO_HARDWAREGAIN, gains[field]), 0);
		afe_registers(6, field, 0, 0, 0, 0, 0, 0, &r);
		for (int c = 0; c < 8; c++)
			same = same && (fake.reg[R_CH1SET + c] & 0x70) == (r.chset[c] & 0x70);
		CHECK_MSG(same, "o campo PGA difere do M33");
		CHECK_EQ(read_raw(IIO_CHAN_INFO_HARDWAREGAIN, &val, &val2), IIO_VAL_INT);
		CHECK_EQ(val, gains[field]);
	}
	CHECK_EQ(write_raw(IIO_CHAN_INFO_HARDWAREGAIN, 3), -EINVAL);
	CHECK_EQ(write_raw(IIO_CHAN_INFO_HARDWAREGAIN, 24), 0);

	CHECK(store("test_signal", "1x_slow") > 0);
	afe_registers(6, 6, 1, 0, 0, 0, 0, 0, &r);
	CHECK_EQ(fake.reg[R_CONFIG2], r.config2);
	CHECK(!strcmp(show("test_signal"), "1x_slow\n"));
	CHECK(store("test_signal", "off") > 0);
	CHECK_EQ(fake.reg[R_CONFIG2], 0xc0);
	CHECK(store("test_signal", "loud") < 0);

	CHECK(store("input_mux", "shorted") > 0);
	afe_registers(6, 6, 0, 0, 0, 0, 1, 0, &r);
	{
		bool same = true;

		for (int c = 0; c < 8; c++)
			same = same && (fake.reg[R_CH1SET + c] & 0x07) == (r.chset[c] & 0x07);
		CHECK_MSG(same, "o MUX em curto difere do M33");
	}
	CHECK(!strcmp(show("input_mux"), "shorted\n"));
	CHECK(store("input_mux", "normal") > 0);

	{
		int val = -1, val2 = -1;

		/* One step at gain 24, in millivolts: the text the framework was
		 * found to be off the grid by (BRINGUP_AFE.md section 6.8). */
		CHECK_EQ(read_raw(IIO_CHAN_INFO_SCALE, &val, &val2), IIO_VAL_INT_PLUS_NANO);
		CHECK_EQ(val, 0);
		CHECK_EQ(val2, 22351);
	}
}

static void test_single_read(void)
{
	int val = 0, val2 = 0;

	section("leitura avulsa (sem buffer), pelo caminho do link sem interrupção");
	fake_power_up();
	for (int c = 0; c < 8; c++)
		fake.input_uv[c] = 1000.0 * (c + 1) * (c % 2 ? -1 : 1);
	if (probe(0))
		return;
	CHECK_EQ(read_raw(IIO_CHAN_INFO_RAW, &val, &val2), IIO_VAL_INT);
	CHECK_EQ(val, fake_code(0));
	CHECK(fake.reg[R_CONFIG4] & 0x08);	/* single shot, for this read */
}

static void test_buffer_host_timer(void)
{
	bool exact = true;

	section("buffer pelo timer do host (o link usb)");
	fake_power_up();
	for (int c = 0; c < 8; c++)
		fake.input_uv[c] = 1500.0 * (c - 4) + 7.0;
	if (probe(0))
		return;
	buffer_on();
	CHECK(fake.started);
	CHECK(!(fake.reg[R_CONFIG4] & 0x08));	/* continuous */
	CHECK(priv()->timer.active);

	for (int i = 0; i < 20; i++) {
		fake_convert();
		tick();
		CHECK_EQ(shim_spi_pending(), 1);
		shim_spi_complete_one();
	}
	CHECK_EQ(shim_scan_count, 20);
	for (unsigned int s = 0; s < shim_scan_count; s++) {
		exact = exact && shim_scans[s].count == 8;
		for (int c = 0; c < 8; c++)
			exact = exact && code24(shim_scans[s].value[c]) == fake_code((unsigned int)c);
	}
	CHECK_MSG(exact, "um código do buffer não é o que o conversor converteu");
	CHECK(shim_scans[1].timestamp > shim_scans[0].timestamp);

	/* Three samples due while one transfer is in flight: one is lost, and
	 * the driver says so. */
	shim_scan_count = 0;
	tick();
	tick();
	tick();
	while (shim_spi_complete_one())
		;
	CHECK_EQ(shim_scan_count, 2);
	CHECK(!strcmp(show("lost_samples"), "1\n"));

	/* The part converts on its own clock; the host asks when its timer
	 * says. Five conversions and one tick: four were never read, and
	 * nothing counts them. */
	{
		const u64 made_before = fake.conversions;
		const long lost_before = strtol(show("lost_samples"), NULL, 10);
		long unaccounted;

		shim_scan_count = 0;
		for (int i = 0; i < 5; i++)
			fake_convert();
		tick();
		shim_spi_complete_one();
		/* Every conversion is either a scan or a counted loss; whatever is
		 * neither vanished without a mark. */
		unaccounted = (long)(fake.conversions - made_before) - (long)shim_scan_count -
			      (strtol(show("lost_samples"), NULL, 10) - lost_before);
		DEFECT(unaccounted > 0,
		       "conversões que o conversor fez e ninguém leu não são contadas no link sem "
		       "interrupção (o contador do GP6 é o único que as vê)",
		       "usb_parity, fase 5");
	}

	/* An electrode off, with sensing enabled behind the driver's back
	 * (the debug register access): the part reports it in every status
	 * word, and the scan carries no status. */
	(void)shim_iio->info->debugfs_reg_access(shim_iio, R_LOFF_SENSP, 0xff, NULL);
	fake.detached_p = 0x04;
	fake_convert();
	shim_scan_count = 0;
	tick();
	shim_spi_complete_one();
	CHECK_EQ(shim_scan_count, 1);
	DEFECT(shim_iio->num_channels == 9,
	       "a palavra de status, com o estado de lead-off de cada amostra, é lida e "
	       "descartada: nenhum canal do buffer a leva",
	       "usb_parity, fase 2");
	(void)shim_iio->info->debugfs_reg_access(shim_iio, R_LOFF_SENSP, 0x00, NULL);

	buffer_off();
	CHECK(!fake.started);
	CHECK(!priv()->timer.active);
}

static void test_buffer_square(void)
{
	int last = 0, halves = 0, wrong = 0, run = 0;

	section("o gerador interno pelo buffer: cada meio período exato");
	fake_power_up();
	if (probe(0))
		return;
	CHECK(store("test_signal", "1x_slow") > 0);
	CHECK(store("input_mux", "test_signal") > 0);
	buffer_on();
	for (int i = 0; i < 2048; i++) {
		fake_convert();
		tick();
		shim_spi_complete_one();
	}
	CHECK_EQ(shim_scan_count, 2048);
	for (unsigned int s = 0; s < shim_scan_count; s++) {
		const int sign = code24(shim_scans[s].value[0]) > 0 ? 1 : -1;

		if (s && sign != last) {
			if (halves++ && run != 128)
				wrong++;
			run = 0;
		}
		run++;
		last = sign;
	}
	/* At 250 SPS a half period of f_CLK/2^21 is 128 conversions. */
	CHECK(halves >= 14);
	CHECK_EQ(wrong, 0);
	buffer_off();
}

static void test_buffer_drdy(void)
{
	section("buffer pela interrupção do DRDY (o link spi)");
	fake_power_up();
	if (probe(42))
		return;
	CHECK(!strcmp(show("timestamp_source"), "drdy_irq\n"));
	CHECK(shim_irq_handler != NULL);
	buffer_on();
	fake_convert();
	CHECK_EQ(shim_irq_handler(42, shim_irq_dev), IRQ_HANDLED);
	CHECK_EQ(shim_spi_pending(), 1);
	shim_spi_complete_one();
	CHECK_EQ(shim_scan_count, 1);
	buffer_off();
}

static void test_write_not_taken(void)
{
	section("uma escrita que o conversor não guardou");
	fake_power_up();
	if (probe(0))
		return;
	/* SRB1 stuck open: the part keeps MISC1 bit 5 at 0 whatever is written. */
	fake.stuck_reg = R_MISC1;
	fake.stuck_mask = 0x20;
	CHECK(store("montage", "referential") > 0);
	DEFECT(fake.reg[R_MISC1] == 0 && !strcmp(show("montage"), "referential\n"),
	       "a montagem referencial é aceita e relatada sem estar no chip: o cache do "
	       "regmap responde no lugar dele",
	       "usb_parity, fase 3");
}

int main(void)
{
	printf("ti-ads1299 - verificações de host\n");
	test_probe();
	test_probe_refusals();
	test_against_m33();
	test_single_read();
	test_buffer_host_timer();
	test_buffer_square();
	test_buffer_drdy();
	test_write_not_taken();

	printf("\nNão coberto por esta suíte:\n"
	       "  - tempo, concorrência e contexto de interrupção (o shim não tem nenhum)\n"
	       "  - ruído, assentamento e o caminho analógico (o conversor falso não tem)\n"
	       "  - as constantes do datasheet: o falso foi escrito pela mesma leitura que o\n"
	       "    driver, exceto onde o silício já respondeu (BRINGUP_AFE.md §4) e onde o\n"
	       "    M33 é o oráculo\n");
	return report();
}
