// SPDX-License-Identifier: GPL-2.0-only
//
// The fake device, and the globals the shim needs. See fake_mcp2210.h for what
// this is and is not evidence for.

#include <stdarg.h>

#include "fake_mcp2210.h"

struct fake_mcp2210 fake;

unsigned long shim_usleep_calls;
int shim_hid_parse_ret, shim_hid_start_ret, shim_hid_open_ret;

char shim_log[SHIM_LOG_LINES][SHIM_LOG_LEN];
unsigned int shim_log_count;

void shim_log_reset(void)
{
	shim_log_count = 0;
	memset(shim_log, 0, sizeof(shim_log));
}

void shim_logf(const char *level, const char *fmt, ...)
{
	va_list ap;

	(void)level;
	if (shim_log_count >= SHIM_LOG_LINES)
		return;
	va_start(ap, fmt);
	vsnprintf(shim_log[shim_log_count], SHIM_LOG_LEN, fmt, ap);
	va_end(ap);
	shim_log_count++;
}

bool shim_log_contains(const char *needle)
{
	unsigned int i;

	for (i = 0; i < shim_log_count; i++)
		if (strstr(shim_log[i], needle))
			return true;
	return false;
}

void fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));

	/*
	 * A device as it comes out of the box. Section 1.3.2: 1 Mbit, four
	 * bytes per transaction, GP1 as the chip select line. Everything else
	 * is a general purpose pin, which is what makes the chip select check
	 * in the driver worth having - the default is not GP0.
	 */
	fake.pin_designation[1] = FAKE_PIN_CS;
	fake.bitrate = 1000000;
	fake.xfer_bytes = 4;
	fake.idle_cs = 0x01ff;
	fake.active_cs = 0x01ff;
	fake.gpio_dir = 0x01ff;		/* all inputs */
	fake.gpio_default_dir = 0x01ff;
	fake.gpio_default_value = 0x0000;

	shim_usleep_calls = 0;
	shim_hid_parse_ret = shim_hid_start_ret = shim_hid_open_ret = 0;
	shim_log_reset();
}

/* --------------------------------------------------------------- replies */

/* Status bytes, section 3.5.1.1 and 3.6.2 */
#define ST_OK			0x00
#define ST_BUS_UNAVAILABLE	0xf7
#define ST_BUSY			0xf8
#define ST_UNKNOWN_COMMAND	0xf9

/*
 * The SPI engine status byte. All three values are documented, one per
 * response structure: Table 3-60 gives 0x20, Table 3-62 gives 0x30 and Table
 * 3-63 gives 0x10. Spreading three values of one field across three tables is
 * how the driver came to have all three assigned to the wrong name.
 *
 * ENG_UNDOCUMENTED is in no table at all, and a test uses it: a transfer loop
 * that still completes under a status byte it cannot interpret is a loop that
 * does not depend on this field, which is what made those wrong names
 * harmless.
 */
#define ENG_FINISHED		0x10	/* finished, no more data to send */
#define ENG_STARTED_NO_DATA	0x20	/* started, no data to receive */
#define ENG_NOT_FINISHED	0x30	/* not finished, data available */
#define ENG_UNDOCUMENTED	0x77

static void engine_reset(void)
{
	fake.engine_active = false;
	fake.engine_in = 0;
	fake.engine_out = 0;
}

static void build_reply(const u8 *req, u8 *rep)
{
	u8 cmd = req[0];

	memset(rep, 0, 64);
	rep[0] = cmd;
	rep[1] = ST_OK;

	switch (cmd) {
	case 0x10:	/* Get MCP2210 Status, Table 3-70 */
		rep[2] = 0x01;	/* no external request for bus release */
		rep[3] = 0x01;	/* SPI bus owned by the USB bridge */
		break;

	case 0x11:	/* Cancel the current SPI transfer, Table 3-65 */
		engine_reset();
		rep[2] = 0x01;
		rep[3] = 0x00;	/* no owner */
		break;

	case 0x12:	/* Get interrupt event count, Tables 3-56 and 3-57 */
		fake.last_int_count_arg = req[1];
		put_unaligned_le16(fake.int_count, rep + 4);
		/*
		 * Byte 1 is active low: 0x00 reads and then resets, anything
		 * else reads and leaves the counter alone.
		 */
		if (req[1] == 0x00)
			fake.int_count = 0;
		break;

	case 0x20:	/* Get (VM) chip settings, Table 3-39 */
		memcpy(rep + 4, fake.pin_designation, FAKE_NGPIO);
		put_unaligned_le16(fake.gpio_default_value, rep + 13);
		put_unaligned_le16(fake.gpio_default_dir, rep + 15);
		rep[17] = fake.other_settings;
		rep[18] = 0x00;	/* NVRAM not protected */
		break;

	case 0x21:	/* Set (VM) chip settings, Table 3-40 */
		memcpy(fake.last_chip_settings_request, req, 64);
		fake.set_chip_settings_writes++;
		memcpy(fake.pin_designation, req + 4, FAKE_NGPIO);
		fake.gpio_default_value = get_unaligned_le16(req + 13);
		fake.gpio_default_dir = get_unaligned_le16(req + 15);
		fake.other_settings = req[17];
		break;

	case 0x30: {	/* Set (VM) GPIO value, Tables 3-48 and 3-49 */
		u16 want = get_unaligned_le16(req + 4);
		unsigned int i;

		for (i = 0; i < FAKE_NGPIO; i++) {
			if (fake.pin_designation[i] != FAKE_PIN_GPIO)
				continue;
			if (fake.gpio_dir & (u16)BIT(i))
				continue;	/* an input is not driven */
			fake.gpio_value = (u16)((fake.gpio_value & ~(u16)BIT(i)) |
						(want & (u16)BIT(i)));
		}
		put_unaligned_le16(fake.gpio_value, rep + 4);
		break;
	}

	case 0x31:	/* Get (VM) GPIO value, Table 3-47 */
		put_unaligned_le16(fake.gpio_value, rep + 4);
		break;

	case 0x32:	/* Set (VM) GPIO direction, Table 3-44 */
		fake.gpio_dir = get_unaligned_le16(req + 4);
		break;

	case 0x33:	/* Get (VM) GPIO direction, Table 3-43 */
		put_unaligned_le16(fake.gpio_dir, rep + 4);
		break;

	case 0x40:	/* Set (VM) SPI transfer settings, Tables 3-35 to 3-37 */
		/*
		 * Figure 3-13: with a transfer ongoing the settings are NOT
		 * written and the device answers 0xF8. This is the behaviour
		 * that turns one abandoned transaction into a wedged
		 * controller when nobody sends a cancel.
		 */
		if (fake.engine_active) {
			rep[1] = ST_BUSY;
			break;
		}
		fake.bitrate = get_unaligned_le32(req + 4);
		fake.idle_cs = get_unaligned_le16(req + 8);
		fake.active_cs = get_unaligned_le16(req + 10);
		fake.cs_to_data = get_unaligned_le16(req + 12);
		fake.data_to_cs = get_unaligned_le16(req + 14);
		fake.data_to_data = get_unaligned_le16(req + 16);
		fake.xfer_bytes = get_unaligned_le16(req + 18);
		fake.spi_mode = req[20];
		engine_reset();
		break;

	case 0x41:	/* Get (VM) SPI transfer settings, Table 3-34 */
		rep[2] = 17;
		put_unaligned_le32(fake.bitrate, rep + 4);
		put_unaligned_le16(fake.idle_cs, rep + 8);
		put_unaligned_le16(fake.active_cs, rep + 10);
		put_unaligned_le16(fake.xfer_bytes, rep + 18);
		rep[20] = fake.spi_mode;
		break;

	case 0x42: {	/* Transfer SPI data, Tables 3-58 to 3-61 */
		u8 n = req[1];
		size_t avail, give;

		if (fake.bus_unavailable) {
			rep[1] = ST_BUS_UNAVAILABLE;
			break;
		}
		if (fake.busy_before_accept > 0) {
			fake.busy_before_accept--;
			rep[1] = ST_BUSY;
			break;
		}

		if (fake.chunks < FAKE_MAX_CHUNKS)
			fake.chunk_len[fake.chunks] = n;
		fake.chunks++;

		if (n > 60)		/* the host must never ask for more */
			n = 60;
		if (fake.engine_in + n <= FAKE_MAX_MESSAGE)
			memcpy(fake.mosi + fake.engine_in, req + 4, n);
		fake.engine_in += n;
		fake.engine_active = true;

		/* Loopback: what went out on MOSI comes back on MISO. */
		avail = fake.engine_in - fake.engine_out;
		give = min(avail, (size_t)60);
		if (fake.max_chunk_out && give > fake.max_chunk_out)
			give = fake.max_chunk_out;

		if (give && fake.engine_out + give <= FAKE_MAX_MESSAGE)
			memcpy(rep + 4, fake.mosi + fake.engine_out, give);
		fake.engine_out += give;

		/*
		 * A device that claims more bytes than it put in the report.
		 * Without the clamp in the driver this is a buffer overflow,
		 * which is why the injection exists. inflate_last_reply aims it
		 * at the one reply where the overflow lands past rx_flat rather
		 * than merely inside it.
		 */
		if (fake.inflate_last_reply && fake.engine_out >= fake.xfer_bytes)
			rep[2] = 60;
		else
			rep[2] = (u8)(give + (size_t)fake.extra_return_bytes);

		if (fake.weird_engine_status)
			rep[3] = ENG_UNDOCUMENTED;
		else if (fake.engine_out >= fake.xfer_bytes)
			rep[3] = ENG_FINISHED;
		else if (!give)
			rep[3] = ENG_STARTED_NO_DATA;
		else
			rep[3] = ENG_NOT_FINISHED;
		fake.last_engine_status = rep[3];

		if (fake.engine_out >= fake.xfer_bytes)
			engine_reset();
		break;
	}

	case 0x50:
	case 0x51:	/* Read / Write EEPROM memory */
		fake.eeprom_touched = true;
		break;

	case 0x60:
	case 0x61:
	case 0x70:	/* NVRAM settings and the access password */
		fake.nvram_touched = true;
		break;

	default:	/* Table 3-72 */
		rep[1] = ST_UNKNOWN_COMMAND;
		break;
	}

	if (fake.echo_wrong_command)
		rep[0] = (u8)(cmd ^ 0xff);
}

/* ------------------------------------------------------------- the seam */

static void deliver(struct hid_device *hdev, u8 *rep)
{
	struct hid_driver *drv = shim_driver_ref();

	drv->raw_event(hdev, NULL, rep, 64);
}

int hid_hw_output_report(struct hid_device *hdev, u8 *buf, size_t len)
{
	u8 rep[64];
	bool misbehave;

	fake.exchanges++;
	fake.cmd_count[buf[0]]++;

	build_reply(buf, rep);

	misbehave = fake.exchanges >= fake.silent_start;

	if (misbehave && fake.drop_count > 0) {
		/* The answer is lost. Nothing is ever delivered for it. */
		fake.drop_count--;
		return (int)len;
	}

	if (misbehave && fake.hold_count > 0) {
		/*
		 * The answer is late rather than lost: it is queued and will
		 * be delivered in place of the NEXT command's answer, which is
		 * then itself queued. The device stays one exchange behind for
		 * the rest of the session, which is what a slow device does
		 * and what a driver that does not match replies to commands
		 * cannot notice.
		 */
		fake.hold_count--;
		memcpy(fake.held, rep, 64);
		fake.held_valid = true;
		return (int)len;
	}

	if (fake.held_valid) {
		u8 late[64];

		memcpy(late, fake.held, 64);
		memcpy(fake.held, rep, 64);	/* stay one behind */
		deliver(hdev, late);
		return (int)len;
	}

	deliver(hdev, rep);
	return (int)len;
}
