/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A fake MCP2210 that answers 64-byte reports.
 *
 * WHAT IT IS AND IS NOT EVIDENCE FOR. This file was written from
 * docs/Register_Map_MCP2210.md - the same transcription the driver was written
 * from. So every check that asserts a field is at a given offset proves that
 * the driver and this file agree, and nothing else: consistency, not
 * correctness, which is the distinction implementation_plan_afe_bench.md
 * section 13.1 is built around. Only independently sourced vectors - the
 * datasheet's own worked examples, a GPLv2 third-party driver read as an
 * oracle, or a USB analyser capture - would make those into evidence.
 *
 * What IS evidence today is everything that does not depend on the offsets
 * being right: fragmentation at the 60-byte boundary, the stall bound, the
 * accumulation and truncation of received bytes, -EMSGSIZE, the recovery path
 * after a timeout, and whether a reply is matched to the command that asked
 * for it. Those are claims about the code doing what the code says.
 *
 * The SPI side is a loopback: MISO returns what MOSI sent, which is what a
 * jumper between the two pins does on a bench. A test that wants a specific
 * response pattern sets fake.miso_override.
 */

#ifndef MCP2210_TEST_FAKE_H
#define MCP2210_TEST_FAKE_H

#include "kernel_shim_dev.h"

#define FAKE_NGPIO		9
#define FAKE_MAX_MESSAGE	1024
#define FAKE_MAX_CHUNKS		64

/* Pin designations, Table 3-1 */
#define FAKE_PIN_GPIO		0x00
#define FAKE_PIN_CS		0x01
#define FAKE_PIN_DEDICATED	0x02

struct fake_mcp2210 {
	/* --- volatile chip settings, Tables 3-39 and 3-40 --- */
	u8 pin_designation[FAKE_NGPIO];
	u16 gpio_default_value;
	u16 gpio_default_dir;
	u8 other_settings;

	/* --- volatile GPIO, Tables 3-43 to 3-49 --- */
	u16 gpio_value;
	u16 gpio_dir;

	/* --- SPI transfer settings, Table 3-35 --- */
	u32 bitrate;
	u16 idle_cs, active_cs;
	u16 cs_to_data, data_to_cs, data_to_data;
	u16 xfer_bytes;
	u8 spi_mode;

	/* --- interrupt event counter, Table 3-56 --- */
	u16 int_count;
	u8 last_int_count_arg;

	/* --- the SPI engine --- */
	bool engine_active;
	size_t engine_in;	/* bytes taken from the host */
	size_t engine_out;	/* bytes handed back to the host */
	u8 mosi[FAKE_MAX_MESSAGE];

	/* --- injectable behaviour --- */
	int busy_before_accept;	 /* answer 0xF8 to this many 0x42 */
	bool bus_unavailable;	 /* answer 0xF7 to 0x42 */
	unsigned int silent_start;	/* first exchange to misbehave on */
	int drop_count;		 /* discard the reply: nothing is delivered */
	int hold_count;		 /* queue the reply: it arrives one exchange late */
	bool echo_wrong_command; /* corrupt byte 0 of every reply */
	unsigned int max_chunk_out;	/* cap bytes returned per 0x42, 0 = no cap */
	int extra_return_bytes;	 /* claim this many MORE bytes than are available */
	bool inflate_last_reply; /* claim a full 60 on the reply that finishes */
	bool weird_engine_status;/* report a status byte the driver does not know */

	/* --- observation --- */
	unsigned int cmd_count[256];
	unsigned int exchanges;
	u8 chunk_len[FAKE_MAX_CHUNKS];	/* the sequence of 0x42 payload lengths */
	unsigned int chunks;
	bool nvram_touched;
	bool eeprom_touched;
	unsigned int set_chip_settings_writes;
	u8 last_chip_settings_request[64];

	/* --- internal: the one-deep reply queue that models lag --- */
	u8 held[64];
	bool held_valid;
};

extern struct fake_mcp2210 fake;

/* Reset to a plausible factory device: GP1 is the chip select (section 1.3.2). */
void fake_reset(void);

#endif /* MCP2210_TEST_FAKE_H */
