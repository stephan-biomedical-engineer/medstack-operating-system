// SPDX-License-Identifier: GPL-2.0-only
//
// Host-side tests for the MCP2210 bridge driver - Phase 7 of
// docs/implementation_plan_mcp2210.md.
//
// The driver source is compiled here BYTE FOR BYTE as it ships, with no test
// macro and no seam of its own: the substitution happens below it, in
// hid_hw_output_report(). What that buys is that mcp2210_command(), the status
// decoding and mcp2210_raw_event() are all under test rather than replaced.
//
// Read fake_mcp2210.h before believing any number here. Half of these checks
// are evidence and half are consistency, and the file says which is which.

#include "check.h"
#include "fake_mcp2210.h"

/* The driver, unmodified. */
#include "../../linux-med/drivers/hid/hid-mcp2210.c"

struct check_counters counters;

/* ------------------------------------------------------------- scaffolding */

static struct hid_device hdev;
static struct mcp2210 *mcp;

static int do_probe(void)
{
	int ret;

	memset(&hdev, 0, sizeof(hdev));
	ret = mcp2210_probe(&hdev, NULL);
	mcp = ret ? NULL : hid_get_drvdata(&hdev);
	return ret;
}

static void do_release(void)
{
	shim_devm_release(&hdev.dev);
	mcp = NULL;
}

/*
 * Some injections deliberately leave the bridge mid-transaction, and a bridge
 * mid-transaction refuses the next 0x40 (Figure 3-13). Sub-tests that follow
 * one of those start from a device that is not in that state, so that a
 * failure means what it says instead of meaning "the previous check poisoned
 * this one".
 */
static void fresh(void);

static void clear_obs(void)
{
	fake.chunks = 0;
	fake.exchanges = 0;
	memset(fake.cmd_count, 0, sizeof(fake.cmd_count));
	memset(fake.chunk_len, 0, sizeof(fake.chunk_len));
	shim_usleep_calls = 0;
	shim_log_reset();
}

/* A device whose GP6 is the dedicated interrupt pin, so probe writes 0x21. */
static void fake_reset_with_int_pin(void)
{
	fake_reset();
	fake.pin_designation[MCP2210_INTERRUPT_PIN] = FAKE_PIN_DEDICATED;
}

static void fresh(void)
{
	do_release();
	fake_reset();
	spi_chip_select = 1;
	memset(&hdev, 0, sizeof(hdev));
	CHECK_EQ(mcp2210_probe(&hdev, NULL), 0);
	mcp = hid_get_drvdata(&hdev);
	clear_obs();
}

static int run_message(struct spi_transfer *xs, int n)
{
	struct spi_message msg;
	int i;

	memset(&msg, 0, sizeof(msg));
	INIT_LIST_HEAD(&msg.transfers);
	msg.spi = mcp->child;
	for (i = 0; i < n; i++)
		list_add_tail(&xs[i].transfer_list, &msg.transfers);

	mcp->ctlr->transfer_one_message(mcp->ctlr, &msg);
	return msg.status;
}

static int run_one(const void *tx, void *rx, unsigned int len)
{
	struct spi_transfer x;

	memset(&x, 0, sizeof(x));
	x.tx_buf = tx;
	x.rx_buf = rx;
	x.len = len;
	return run_message(&x, 1);
}

static void fill_pattern(u8 *b, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		b[i] = (u8)(i * 7 + 3);
}

static int read_interrupt_count(void)
{
	char buf[64] = { 0 };
	ssize_t n = dev_attr_interrupt_count.show(&hdev.dev,
						  &dev_attr_interrupt_count, buf);

	return n < 0 ? (int)n : atoi(buf);
}

static int reset_interrupt_count(void)
{
	ssize_t n = dev_attr_interrupt_count_reset.store(&hdev.dev,
					&dev_attr_interrupt_count_reset, "1", 1);

	return n < 0 ? (int)n : 0;
}

/*
 * The negative test, in the form it took when the driver's goal became full
 * coverage. It used to be "no NVRAM command was ever sent", which was
 * guaranteed by the driver not knowing how. Reading the power-up settings is
 * now something the driver does at probe and should. What must never happen is
 * a WRITE, and that is a property of the path rather than of the codebase -
 * which is the stronger claim of the two.
 */
static void check_nvram_not_written(void)
{
	CHECK_MSG(!fake.nvram_written,
		  "nenhuma escrita de NVRAM nem envio de senha");
	CHECK_MSG(!fake.eeprom_touched, "nenhum comando de EEPROM foi emitido");
	CHECK_EQ(fake.cmd_count[0x60] + fake.cmd_count[0x70] +
		 fake.cmd_count[0x50] + fake.cmd_count[0x51], 0);
}

/* ============================================================== A. probe */

static void test_probe(void)
{
	u16 value_before, dir_before;
	int ret;

	section("A. Probe, designacao de chip select e NVRAM intocada");

	/* A1 - a factory device whose chip select really is GP1 */
	fake_reset();
	spi_chip_select = 1;
	ret = do_probe();
	CHECK_EQ(ret, 0);
	CHECK_MSG(mcp && mcp->child, "o filho SPI foi registrado");
	CHECK_EQ(fake.pin_designation[1], FAKE_PIN_CS);
	CHECK_MSG(fake.set_chip_settings_writes == 0,
		  "nada foi reescrito: o pino ja era chip select e GP6 nao e dedicado");
	check_nvram_not_written();
	do_release();

	/*
	 * A2 - the Phase 2 injection, which had never been run. GP6 is the
	 * dedicated interrupt pin, so probe writes the volatile chip settings.
	 * The GPIO default value and direction must come out the other side.
	 */
	fake_reset_with_int_pin();
	fake.gpio_default_value = 0x0155;
	fake.gpio_default_dir = 0x00aa;
	value_before = fake.gpio_default_value;
	dir_before = fake.gpio_default_dir;
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	CHECK_MSG(fake.set_chip_settings_writes == 1,
		  "exatamente um 0x21, o que liga a contagem de bordas");
	CHECK_EQ(fake.gpio_default_value, value_before);
	CHECK_EQ(fake.gpio_default_dir, dir_before);
	CHECK_MSG((fake.other_settings & MCP2210_MASK_INT_MODE) ==
		  FIELD_PREP(MCP2210_MASK_INT_MODE, MCP2210_INT_MODE_FALLING),
		  "a contagem de bordas de descida foi programada");
	check_nvram_not_written();
	do_release();

	/* A3 - the Phase 3.4 injection: the asked-for pin is a plain GPIO */
	fake_reset();
	spi_chip_select = 0;		/* factory default designates GP1 */
	CHECK_EQ(do_probe(), 0);
	CHECK_EQ(fake.pin_designation[0], FAKE_PIN_CS);
	CHECK_MSG(shim_log_contains("GP0"),
		  "a mensagem nomeia o pino que foi reivindicado");
	check_nvram_not_written();
	do_release();

	/* A4 - a pin with a dedicated function is refused, and named */
	fake_reset_with_int_pin();
	spi_chip_select = MCP2210_INTERRUPT_PIN;
	CHECK_EQ(do_probe(), -EBUSY);
	CHECK_MSG(shim_log_contains("GP6"), "a recusa nomeia o pino");
	CHECK_EQ(fake.pin_designation[MCP2210_INTERRUPT_PIN], FAKE_PIN_DEDICATED);
	do_release();

	/* A5 - GP8 has no chip select designation at all */
	fake_reset();
	spi_chip_select = 8;
	CHECK_EQ(do_probe(), -EINVAL);
	do_release();

	/* A6 - and the bridge with nothing behind it still comes up */
	fake_reset();
	spi_chip_select = 1;
	spi_device = "";
	CHECK_EQ(do_probe(), 0);
	CHECK_MSG(mcp && !mcp->child, "nenhum filho, e o probe passa");
	do_release();
	spi_device = "ads1299";
}

/* =================================================== B. interrupt counter */

static void test_interrupt_counter(void)
{
	int first, second;

	section("B. Contador de eventos de interrupcao (Fase 1)");

	fake_reset_with_int_pin();
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);

	/*
	 * B1 - the injection the plan asks for: two consecutive reads with no
	 * edges in between must return the same number. Before the fix each
	 * read consumed the counter and the second returned zero.
	 */
	fake.int_count = 1234;
	first = read_interrupt_count();
	second = read_interrupt_count();
	CHECK_EQ(first, 1234);
	CHECK_EQ(second, 1234);
	CHECK_MSG(fake.last_int_count_arg != 0,
		  "a leitura pede explicitamente para NAO repor");

	/* B2 - and the reset attribute is the one that resets */
	CHECK_EQ(reset_interrupt_count(), 0);
	CHECK_EQ(fake.last_int_count_arg, 0);
	CHECK_EQ(read_interrupt_count(), 0);

	/* B3 - a counter that keeps counting is still readable twice */
	fake.int_count = 65535;
	CHECK_EQ(read_interrupt_count(), 65535);
	CHECK_EQ(read_interrupt_count(), 65535);

	check_nvram_not_written();
	do_release();
}

/* ====================================================== C. the transfer */

static void test_fragmentation(void)
{
	static const unsigned int sizes[] = { 1, 59, 60, 61, 120, 512 };
	u8 tx[512], rx[512];
	unsigned int i, s;

	section("C. Fragmentacao, acumulacao e truncamento");

	fake_reset();
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);

	/* C1 - the chunk sequence at and around the 60-byte boundary */
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		unsigned int expect_chunks, j, left;

		s = sizes[i];
		clear_obs();
		fill_pattern(tx, s);
		memset(rx, 0, sizeof(rx));
		CHECK_EQ(run_one(tx, rx, s), 0);

		expect_chunks = (s + 59) / 60;
		CHECK_EQ(fake.chunks, expect_chunks);

		left = s;
		for (j = 0; j < expect_chunks && j < FAKE_MAX_CHUNKS; j++) {
			unsigned int want = left > 60 ? 60 : left;

			CHECK_EQ(fake.chunk_len[j], want);
			left -= want;
		}
		CHECK_MSG(memcmp(tx, rx, s) == 0,
			  "o laco de retorno devolveu exatamente o que foi enviado");
		CHECK_MSG(fake.last_engine_status == 0x10,
			  "a ultima resposta trouxe o estado 'concluido' da Tabela 3-63");
	}

	/* C2 - above the declared maximum, and the bridge is never contacted */
	clear_obs();
	CHECK_EQ(run_one(tx, rx, MCP2210_MAX_MESSAGE_BYTES + 1), -EMSGSIZE);
	CHECK_EQ(fake.exchanges, 0);

	/* C3 - a message of no bytes is not a conversation */
	clear_obs();
	CHECK_EQ(run_one(tx, rx, 0), 0);
	CHECK_EQ(fake.exchanges, 0);

	/* C4 - a device that dribbles bytes back a few at a time */
	fresh();
	fake.max_chunk_out = 7;
	fill_pattern(tx, 120);
	memset(rx, 0, sizeof(rx));
	CHECK_EQ(run_one(tx, rx, 120), 0);
	CHECK_MSG(memcmp(tx, rx, 120) == 0,
		  "nenhum byte perdido nem duplicado na acumulacao");
	fake.max_chunk_out = 0;

	/*
	 * C5 - a device that claims a full 60 bytes on the reply that finishes
	 * a 512-byte transfer, when only 32 were left. Two things are checked
	 * at once. The count is impossible, so the transfer is refused rather
	 * than trimmed and carried on with. And rx_flat is the field
	 * immediately before pin_designation in struct mcp2210, so without the
	 * bound at "received + got > len" those 28 extra bytes land on the pin
	 * designations: checking them is checking for the overflow.
	 */
	fresh();
	fake.inflate_last_reply = true;
	fill_pattern(tx, 512);
	memset(rx, 0, sizeof(rx));
	CHECK_EQ(run_one(tx, rx, 512), -EPROTO);
	CHECK_EQ(mcp->pin_designation[0], FAKE_PIN_GPIO);
	CHECK_EQ(mcp->pin_designation[1], FAKE_PIN_CS);
	CHECK_EQ(mcp->pin_designation[2], FAKE_PIN_GPIO);
	CHECK_MSG(fake.cmd_count[MCP2210_CMD_SPI_CANCEL] >= 1,
		  "e a ponte foi devolvida ao repouso antes de sair");
	fake.inflate_last_reply = false;

	/* C6 - several transfers in one message keep their boundaries */
	{
		u8 a_tx[10], b_tx[20], c_tx[30];
		u8 a_rx[10], b_rx[20], c_rx[30];
		struct spi_transfer xs[3];

		fresh();
		fill_pattern(a_tx, 10);
		fill_pattern(b_tx, 20);
		fill_pattern(c_tx, 30);
		memset(a_rx, 0, sizeof(a_rx));
		memset(b_rx, 0, sizeof(b_rx));
		memset(c_rx, 0, sizeof(c_rx));
		memset(xs, 0, sizeof(xs));
		xs[0].tx_buf = a_tx; xs[0].rx_buf = a_rx; xs[0].len = 10;
		xs[1].tx_buf = b_tx; xs[1].rx_buf = b_rx; xs[1].len = 20;
		xs[2].tx_buf = c_tx; xs[2].rx_buf = c_rx; xs[2].len = 30;

		CHECK_EQ(run_message(xs, 3), 0);
		CHECK_EQ(fake.chunks, 1);
		CHECK_EQ(fake.chunk_len[0], 60);
		CHECK_MSG(memcmp(a_tx, a_rx, 10) == 0, "fronteira 1 preservada");
		CHECK_MSG(memcmp(b_tx, b_rx, 20) == 0, "fronteira 2 preservada");
		CHECK_MSG(memcmp(c_tx, c_rx, 30) == 0, "fronteira 3 preservada");
	}

	/*
	 * C7 - the transfer completes even when the engine status byte is a
	 * value that appears in no table. That is the property that makes the
	 * disputed constants of section 3.5 harmless today: the loop is driven
	 * by byte counts, not by that byte.
	 */
	fresh();
	fake.weird_engine_status = true;
	fill_pattern(tx, 120);
	memset(rx, 0, sizeof(rx));
	CHECK_EQ(run_one(tx, rx, 120), 0);
	CHECK_MSG(memcmp(tx, rx, 120) == 0,
		  "o laco nao depende do byte de estado do motor");
	fake.weird_engine_status = false;

	check_nvram_not_written();
	do_release();
}

/* ================================ G. the power-up settings (phase 6A) */

static void test_nvram_read(void)
{
	section("G. Os ajustes de arranque em NVRAM (Fase 6A)");

	/* G1 - probe reads the power-up settings, and does not write them */
	fresh();
	CHECK_MSG(fake.nvram_read, "o probe leu os ajustes de arranque");
	CHECK_MSG(!fake.nvram_written, "e nao escreveu nenhum");
	check_nvram_not_written();
	do_release();

	/*
	 * G2 - a part whose NVRAM designates a different pin than the one in
	 * use. This works, and it means the device depends on this driver
	 * correcting it on every boot, so it is said out loud.
	 */
	fake_reset();
	fake.nvram_designation[1] = FAKE_PIN_GPIO;	/* RAM still says CS */
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	CHECK_MSG(shim_log_contains("only in RAM"),
		  "o desacordo entre RAM e NVRAM foi relatado");
	CHECK_MSG(shim_log_contains("GP1"), "e nomeia o pino");
	CHECK_MSG(!fake.nvram_written, "relatar nao e consertar: NVRAM intocada");
	do_release();

	/* G3 - and when they agree, nothing is said */
	fresh();
	CHECK_MSG(!shim_log_contains("only in RAM"),
		  "nada a relatar quando RAM e NVRAM concordam");
	do_release();

	/* G4 - the access control state reaches the log */
	fake_reset();
	fake.nvram_access_control = 0x80;	/* permanently locked */
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	CHECK_MSG(shim_log_contains("permanently locked"),
		  "uma peca travada de fabrica e anunciada no probe");
	do_release();

	fake_reset();
	fake.nvram_access_control = 0x40;	/* password protected */
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	CHECK_MSG(shim_log_contains("password protected"), "idem para senha");
	do_release();

	/*
	 * G5 - the sub-command echo. One command code covers five operations,
	 * so byte 0 alone cannot tell two of them apart; byte 2 is the rest of
	 * the correlation. A reply carrying the wrong sub-command is a stray.
	 */
	fake_reset();
	fake.wrong_subcmd_echo = true;
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	mcp = hid_get_drvdata(&hdev);
	CHECK_MSG(mcp->stray_replies >= 1,
		  "uma resposta com o sub-comando errado foi descartada");
	CHECK_MSG(!shim_log_contains("only in RAM"),
		  "e nada foi concluido a partir dela");
	fake.wrong_subcmd_echo = false;
	do_release();
}

/* ============================ F. the SPI core contract (phase 5) */

static void test_spi_contract(void)
{
	u8 a_tx[10], b_tx[20], a_rx[10], b_rx[20];
	struct spi_transfer xs[2];

	section("F. O contrato do nucleo SPI (Fase 5)");

	fresh();
	fill_pattern(a_tx, 10);
	fill_pattern(b_tx, 20);

	/* F1 - a delay on the last transfer becomes the CS hold, rounded up */
	memset(xs, 0, sizeof(xs));
	xs[0].tx_buf = a_tx; xs[0].rx_buf = a_rx; xs[0].len = 10;
	xs[0].delay.value = 250;
	xs[0].delay.unit = SPI_DELAY_UNIT_USECS;
	CHECK_EQ(run_message(xs, 1), 0);
	CHECK_MSG(fake.last_cs_hold_quanta == 3,
		  "250 us viram 3 quanta de 100 us: arredondado PARA CIMA");

	/* F2 - and 2 us, which the hardware cannot express, costs one quantum */
	fresh();
	memset(xs, 0, sizeof(xs));
	xs[0].tx_buf = a_tx; xs[0].rx_buf = a_rx; xs[0].len = 10;
	xs[0].delay.value = 2;
	xs[0].delay.unit = SPI_DELAY_UNIT_USECS;
	CHECK_EQ(run_message(xs, 1), 0);
	CHECK_EQ(fake.last_cs_hold_quanta, 1);

	/* F3 - no delay asked for is no delay programmed */
	fresh();
	memset(xs, 0, sizeof(xs));
	xs[0].tx_buf = a_tx; xs[0].rx_buf = a_rx; xs[0].len = 10;
	CHECK_EQ(run_message(xs, 1), 0);
	CHECK_EQ(fake.last_cs_hold_quanta, 0);

	/*
	 * F4 - a delay in the MIDDLE of a flattened message has nowhere to go,
	 * and is refused rather than accumulated or moved to the end. A delay
	 * that happens somewhere else is not the delay that was asked for.
	 */
	fresh();
	memset(xs, 0, sizeof(xs));
	xs[0].tx_buf = a_tx; xs[0].rx_buf = a_rx; xs[0].len = 10;
	xs[0].delay.value = 100;
	xs[0].delay.unit = SPI_DELAY_UNIT_USECS;
	xs[1].tx_buf = b_tx; xs[1].rx_buf = b_rx; xs[1].len = 20;
	CHECK_EQ(run_message(xs, 2), -EINVAL);
	CHECK_EQ(fake.exchanges, 0);

	/* F5 - cs_change mid-message is the opposite of one transaction */
	fresh();
	memset(xs, 0, sizeof(xs));
	xs[0].tx_buf = a_tx; xs[0].rx_buf = a_rx; xs[0].len = 10;
	xs[0].cs_change = 1;
	xs[1].tx_buf = b_tx; xs[1].rx_buf = b_rx; xs[1].len = 20;
	CHECK_EQ(run_message(xs, 2), -EINVAL);
	CHECK_EQ(fake.exchanges, 0);

	/* F6 - but cs_change on the LAST transfer is what every message means */
	fresh();
	memset(xs, 0, sizeof(xs));
	xs[0].tx_buf = a_tx; xs[0].rx_buf = a_rx; xs[0].len = 10;
	xs[0].cs_change = 1;
	CHECK_EQ(run_message(xs, 1), 0);

	/* F7 - GP8 is an input, permanently, and says so */
	CHECK_EQ(mcp->gc.direction_output(&mcp->gc, 8, 1), -EIO);
	CHECK_EQ(mcp->gc.get_direction(&mcp->gc, 8), GPIO_LINE_DIRECTION_IN);
	CHECK_EQ(mcp->gc.direction_input(&mcp->gc, 8), 0);
	clear_obs();
	mcp->gc.set(&mcp->gc, 8, 1);
	CHECK_MSG(fake.exchanges == 0,
		  "e escrever nele nao fala com a ponte: o byte alto e don't care");

	/* F8 - the other eight can still be outputs */
	CHECK_EQ(mcp->gc.direction_output(&mcp->gc, 3, 1), 0);
	CHECK_EQ(mcp->gc.get_direction(&mcp->gc, 3), GPIO_LINE_DIRECTION_OUT);

	check_nvram_not_written();
	do_release();
}

/* ==================================================== D. the stall bound */

static void test_stall_bound(void)
{
	u8 tx[64], rx[64];

	section("D. Limite de estagnacao");

	fake_reset();
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	fill_pattern(tx, 8);

	/* D1 - 99 refusals then acceptance: the transfer completes */
	clear_obs();
	fake.busy_before_accept = 99;
	memset(rx, 0, sizeof(rx));
	CHECK_EQ(run_one(tx, rx, 8), 0);
	CHECK_MSG(memcmp(tx, rx, 8) == 0, "os dados chegaram depois das recusas");
	CHECK_EQ(shim_usleep_calls, 99);

	/* D2 - refusals without end: bounded, and it gives up */
	clear_obs();
	fake.busy_before_accept = 100000;
	CHECK_EQ(run_one(tx, rx, 8), -ETIMEDOUT);
	CHECK_MSG(shim_usleep_calls <= 101,
		  "o laco e limitado: uma ponte travada vira um erro, nao um pendura");
	fake.busy_before_accept = 0;

	/*
	 * D3 - 0xF7 is "the SPI bus belongs to an external host", which is not
	 * the same condition as 0xF8 "the engine is busy" and does not clear by
	 * waiting. The driver maps both to -EBUSY and retries.
	 */
	fresh();
	fake.bus_unavailable = true;
	CHECK_EQ(run_one(tx, rx, 8), -EBUSY);
	CHECK_MSG(shim_usleep_calls == 0,
		  "e falha de imediato: esperar nao devolve um barramento que tem outro dono");
	fake.bus_unavailable = false;

	/*
	 * D4 - the bound is a budget and not a number. At the controller's own
	 * minimum bitrate a 512-byte transaction clocks for nearly three
	 * seconds, so 500 refusals are still a transfer that is merely slow.
	 * Under the old flat bound of 100 this transfer failed.
	 */
	{
		u8 big_tx[512], big_rx[512];
		struct spi_transfer slow;

		fresh();
		fill_pattern(big_tx, 512);
		memset(big_rx, 0, sizeof(big_rx));
		memset(&slow, 0, sizeof(slow));
		slow.tx_buf = big_tx;
		slow.rx_buf = big_rx;
		slow.len = 512;
		slow.speed_hz = 1500;

		fake.busy_before_accept = 500;
		CHECK_EQ(run_message(&slow, 1), 0);
		CHECK_MSG(memcmp(big_tx, big_rx, 512) == 0,
			  "uma transferencia lenta completa, com os dados certos");
		CHECK_MSG(shim_usleep_calls >= 500,
			  "e o driver realmente esperou as 500 recusas");
		fake.busy_before_accept = 0;
	}

	check_nvram_not_written();
	do_release();
}

/* ============================== E. Fases 3 e 4, ainda nao implementadas */

static void test_unimplemented_phases(void)
{
	u8 tx[128], rx[128];
	int gpio, count;

	section("E. O que as Fases 3 e 4 consertaram");

	/*
	 * E1 - a reply whose echoed command byte is not the command that was
	 * sent is discarded. Every reply echoes byte 0, including an
	 * unsupported command (Table 3-72), which is what makes the check cheap.
	 */
	fake_reset();
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	clear_obs();
	fake.gpio_value = 0x01ab;
	fake.echo_wrong_command = true;
	gpio = mcp->gc.get(&mcp->gc, 0);
	CHECK_EQ(gpio, -ETIMEDOUT);
	CHECK_EQ(mcp->stray_replies, 1);
	fake.echo_wrong_command = false;
	do_release();

	/*
	 * E2 - and the consequence of matching them: a reply that arrives one
	 * exchange late is no longer consumed by whatever asked next. A GPIO
	 * read times out, and its answer is refused by the interrupt counter
	 * instead of being reported as an edge count.
	 */
	fake_reset_with_int_pin();
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	clear_obs();
	fake.gpio_value = 0x01ab;
	fake.int_count = 7;
	fake.misbehave_cmd = MCP2210_CMD_GET_GPIO_VALUE;
	fake.hold_count = 1;

	CHECK_MSG(mcp->gc.get(&mcp->gc, 0) < 0,
		  "a leitura de GPIO sem resposta falha, como deve");
	count = read_interrupt_count();
	CHECK_MSG(count != 0x01ab,
		  "o valor dos pinos NAO virou a contagem de bordas");
	CHECK_EQ(count, -ETIMEDOUT);
	CHECK_MSG(mcp->stray_replies >= 1,
		  "a resposta atrasada foi contada como descartada");
	/*
	 * And the shape of the failure is the point. A device one exchange
	 * behind now produces a run of timeouts - loud, and impossible to
	 * mistake for data - where before it produced plausible wrong numbers.
	 */
	do_release();

	/*
	 * E3 - a device that over-reports the byte count on every reply. This
	 * one was found by the harness and not by reading: the driver used to
	 * trim the count and carry on, which finished the message early and
	 * returned 0 with a buffer holding bytes that were never received. On
	 * this path that buffer becomes a sample and a checksum is computed
	 * over it afterwards, so the corruption would have arrived certified.
	 * It is now refused.
	 */
	fresh();
	fill_pattern(tx, 120);
	fake.extra_return_bytes = 20;
	memset(rx, 0, sizeof(rx));
	CHECK_EQ(run_one(tx, rx, 120), -EPROTO);
	CHECK_MSG(fake.cmd_count[MCP2210_CMD_SPI_CANCEL] >= 1,
		  "um erro de transporte nao deixa a ponte ocupada");
	fake.extra_return_bytes = 0;

	/*
	 * E4 - the one that cost the most. A transfer is abandoned halfway,
	 * and Figure 3-13 says the settings command is refused while a
	 * transaction is ongoing. Before the cancel existed, that single
	 * timeout made the controller useless for the rest of its life.
	 */
	fresh();
	fill_pattern(tx, 120);

	fake.misbehave_cmd = MCP2210_CMD_SPI_TRANSFER;	/* lose one data chunk's reply */
	fake.drop_count = 1;
	CHECK_EQ(run_one(tx, rx, 120), -ETIMEDOUT);
	CHECK_MSG(fake.cmd_count[MCP2210_CMD_SPI_CANCEL] >= 1,
		  "o caminho de erro cancelou a transacao");
	CHECK_MSG(!fake.engine_active,
		  "e a ponte voltou ao repouso em vez de ficar segurando-a");

	clear_obs();
	memset(rx, 0, sizeof(rx));
	CHECK_EQ(run_one(tx, rx, 120), 0);
	CHECK_MSG(memcmp(tx, rx, 120) == 0,
		  "a transferencia seguinte funciona, e com os dados certos");

	/*
	 * E5 - and the belt to that pair of braces. A bridge found already
	 * mid-transaction, because a previous session died without cancelling
	 * or because the cancel itself was lost, is recovered on the spot:
	 * the settings command comes back refused, the driver cancels and
	 * tries once more instead of propagating the refusal forever.
	 */
	fresh();
	fake.engine_active = true;
	fake.xfer_bytes = 120;
	fake.engine_in = 60;
	fake.engine_out = 60;
	fill_pattern(tx, 120);
	memset(rx, 0, sizeof(rx));
	CHECK_EQ(run_one(tx, rx, 120), 0);
	CHECK_MSG(memcmp(tx, rx, 120) == 0, "e os dados estao certos");
	CHECK_EQ(fake.cmd_count[MCP2210_CMD_SPI_CANCEL], 1);

	/* E6 - a bus with an external owner is diagnosed, not just refused */
	fresh();
	fake.bus_unavailable = true;
	CHECK_EQ(run_one(tx, rx, 8), -EBUSY);
	CHECK_MSG(fake.cmd_count[MCP2210_CMD_GET_CHIP_STATUS] >= 1,
		  "o driver perguntou quem e o dono do barramento");
	CHECK_MSG(shim_log_contains("held by"), "e disse quem era no log");
	fake.bus_unavailable = false;

	check_nvram_not_written();
	do_release();
}

/* ------------------------------------------------------------------ main */

int main(void)
{
	printf("MCP2210 - verificacoes de host (Fase 7)\n");

	test_probe();
	test_interrupt_counter();
	test_fragmentation();
	test_stall_bound();
	test_spi_contract();
	test_nvram_read();
	test_unimplemented_phases();

	int status = report();

	/*
	 * Printed on every run, green included. A suite that quietly tested
	 * less than it claims is the failure this repository has already paid
	 * for once, and the cheapest defence is to say out loud what is not
	 * here.
	 */
	printf("\nO que estas verificacoes NAO dizem:\n"
	       "  - nada sobre deslocamentos, opcodes ou bits de modo. O dispositivo\n"
	       "    falso foi escrito a partir da mesma transcricao que o driver, entao\n"
	       "    os dois concordarem prova consistencia, nao correcao.\n"
	       "  - nada sobre silicio. Nenhum modulo foi carregado e nenhuma amostra\n"
	       "    foi adquirida. As constantes foram conferidas contra uma TRANSCRICAO\n"
	       "    do datasheet - que nao e o datasheet, e nao e a peca.\n"
	       "  - nada sobre concorrencia, tempo real ou memoria: o shim nao tem\n"
	       "    threads, nao dorme e nao falha ao alocar.\n"
	       "  - nada sobre o que o hardware faz com o atraso programado. Verifica-se\n"
	       "    que 250 us viram 3 quanta no comando 0x40; que a ponte segure o chip\n"
	       "    select por 300 us e uma afirmacao sobre silicio, e ninguem a mediu.\n"
	       "  - nada sobre a bancada. Isto antecipa a classe de defeito que ela\n"
	       "    encontraria do jeito caro, com um conversor no meio.\n");

	return status;
}
