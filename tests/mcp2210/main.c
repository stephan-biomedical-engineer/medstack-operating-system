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

static void check_no_nvram_or_eeprom(void)
{
	CHECK_MSG(!fake.nvram_touched, "nenhum comando de NVRAM foi emitido");
	CHECK_MSG(!fake.eeprom_touched, "nenhum comando de EEPROM foi emitido");
	CHECK_EQ(fake.cmd_count[0x60] + fake.cmd_count[0x61] +
		 fake.cmd_count[0x70] + fake.cmd_count[0x50] +
		 fake.cmd_count[0x51], 0);
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
	check_no_nvram_or_eeprom();
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
	check_no_nvram_or_eeprom();
	do_release();

	/* A3 - the Phase 3.4 injection: the asked-for pin is a plain GPIO */
	fake_reset();
	spi_chip_select = 0;		/* factory default designates GP1 */
	CHECK_EQ(do_probe(), 0);
	CHECK_EQ(fake.pin_designation[0], FAKE_PIN_CS);
	CHECK_MSG(shim_log_contains("GP0"),
		  "a mensagem nomeia o pino que foi reivindicado");
	check_no_nvram_or_eeprom();
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

	check_no_nvram_or_eeprom();
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
	 * a 512-byte transfer, when only 32 were left. rx_flat is the field
	 * immediately before pin_designation in struct mcp2210, so without the
	 * clamp at "received + got > len" those 28 extra bytes land on the pin
	 * designations: checking them is checking for the overflow.
	 */
	fresh();
	fake.inflate_last_reply = true;
	fill_pattern(tx, 512);
	memset(rx, 0, sizeof(rx));
	CHECK_EQ(run_one(tx, rx, 512), 0);
	CHECK_EQ(mcp->pin_designation[0], FAKE_PIN_GPIO);
	CHECK_EQ(mcp->pin_designation[1], FAKE_PIN_CS);
	CHECK_EQ(mcp->pin_designation[2], FAKE_PIN_GPIO);
	CHECK_MSG(memcmp(tx, rx, 512) == 0, "e os dados continuam corretos");
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

	check_no_nvram_or_eeprom();
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
	clear_obs();
	fake.bus_unavailable = true;
	CHECK_MSG(run_one(tx, rx, 8) != 0, "uma transferencia sem barramento falha");
	DEFECT(shim_usleep_calls > 1,
	       "0xF7 (barramento com dono externo) e repetido 100 vezes como se fosse 0xF8",
	       "plano secao 4.2");
	fake.bus_unavailable = false;

	check_no_nvram_or_eeprom();
	do_release();
}

/* ============================== E. Fases 3 e 4, ainda nao implementadas */

static void test_unimplemented_phases(void)
{
	u8 tx[128], rx[128];
	int gpio, count;

	section("E. O que as Fases 3 e 4 existem para consertar");

	/*
	 * E1 - a reply whose echoed command byte is not the command that was
	 * sent should be discarded. Every reply echoes byte 0, including an
	 * unsupported command (Table 3-72), so the check costs four lines.
	 */
	fake_reset();
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	clear_obs();
	fake.gpio_value = 0x01ab;
	fake.echo_wrong_command = true;
	gpio = mcp->gc.get(&mcp->gc, 0);
	DEFECT(gpio >= 0,
	       "uma resposta com eco divergente e aceita em vez de descartada",
	       "plano secao 2 padrao 1, Fase 3");
	fake.echo_wrong_command = false;
	do_release();

	/*
	 * E2 - and the consequence of not matching replies to commands: a reply
	 * that arrives one exchange late is consumed by whatever asked next.
	 * Here a GPIO read times out and its answer is then handed to the
	 * interrupt counter, which reports the pin values as an edge count.
	 */
	fake_reset_with_int_pin();
	spi_chip_select = 1;
	CHECK_EQ(do_probe(), 0);
	clear_obs();
	fake.gpio_value = 0x01ab;
	fake.int_count = 7;
	fake.silent_start = fake.exchanges + 1;
	fake.hold_count = 1;

	CHECK_MSG(mcp->gc.get(&mcp->gc, 0) < 0,
		  "a leitura de GPIO sem resposta falha, como deve");
	count = read_interrupt_count();
	DEFECT(count == 0x01ab,
	       "uma resposta atrasada de outro comando vira a contagem de bordas",
	       "plano secao 2 padrao 1, Fase 3");
	CHECK_MSG(count == 0x01ab || count == 7,
		  "a leitura devolveu ou o valor contaminado ou o correto");
	do_release();

	/*
	 * E3 - a device that over-reports the byte count on every reply makes
	 * the driver believe the transfer finished early. The driver trusts
	 * byte 2 of the reply without ever comparing it against what it still
	 * owes, so it stops clocking, the bridge is left mid-transaction, and
	 * by E4 below that is unrecoverable. Found by this harness, not by
	 * reading.
	 */
	fresh();
	fill_pattern(tx, 120);
	fake.extra_return_bytes = 20;
	memset(rx, 0, sizeof(rx));
	CHECK_MSG(run_one(tx, rx, 120) == 0,
		  "o driver relata SUCESSO nesta transferencia");
	DEFECT(memcmp(tx, rx, 120) != 0,
	       "uma contagem de recepcao inflada corrompe os dados e o status "
	       "continua 0: um erro de transporte vira um valor plausivel",
	       "achado desta suite, sem secao no plano");
	fake.extra_return_bytes = 0;

	/*
	 * E4 - and the one that costs the most: an abandoned transaction is
	 * never cancelled, and Figure 3-13 says the settings command is refused
	 * while a transfer is ongoing. One timeout and the controller never
	 * works again.
	 */
	fresh();
	fill_pattern(tx, 120);

	fake.silent_start = fake.exchanges + 2;	/* let 0x40 through, drop the first 0x42 */
	fake.drop_count = 1;
	CHECK_EQ(run_one(tx, rx, 120), -ETIMEDOUT);
	CHECK_MSG(fake.engine_active,
		  "a ponte ficou no meio de uma transacao, que e a premissa");

	clear_obs();
	memset(rx, 0, sizeof(rx));
	DEFECT(run_one(tx, rx, 120) != 0,
	       "depois de um timeout a proxima transferencia falha para sempre: "
	       "ninguem envia 0x11",
	       "plano secao 4.1, Fase 4");
	CHECK_EQ(fake.cmd_count[MCP2210_CMD_SPI_CANCEL], 0);
	CHECK_EQ(fake.cmd_count[MCP2210_CMD_GET_CHIP_STATUS], 0);

	check_no_nvram_or_eeprom();
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
	       "  - nada sobre o contrato do nucleo SPI que a Fase 5 cobre: atrasos,\n"
	       "    cs_change e bits_per_word continuam descartados em silencio.\n"
	       "  - nada sobre a bancada. Isto antecipa a classe de defeito que ela\n"
	       "    encontraria do jeito caro, com um conversor no meio.\n");

	return status;
}
