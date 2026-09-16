// SPDX-License-Identifier: GPL-2.0-only
/*
 * MCP2210 - Microchip USB to SPI Host Protocol Bridge
 *
 * Copyright (c) 2026 MedPlatform (TCC)
 *
 * Registers the bridge as a real spi_controller, so that an analogue front-end
 * behind it is an ordinary SPI device and its driver - ti-ads1299 in this
 * layer - is the same code whether the converter hangs off a SoC SPI bus or
 * off a USB cable. That symmetry is the whole reason this file exists; without
 * it the USB link would need a second, parallel implementation of the
 * converter's register map, which is precisely the duplication the platform
 * argues against.
 *
 * Why this is written here rather than adopted
 * --------------------------------------------
 * There is no MCP2210 driver in mainline (checked against the 6.6 tree this
 * repository builds: drivers/hid has hid-mcp2200.c and hid-mcp2221.c and
 * nothing for the 2210). The obvious out-of-tree candidate,
 * daniel-santos/mcp2210-linux, last saw code in 2018 against kernel 4.19 and
 * carries an ioctl/configfs ABI, a self-declared unstable binary configuration
 * format and interrupt emulation - none of which this path needs, all of which
 * would be attack surface inside the kernel's trust domain, and all of which
 * would arrive as SOUP under IEC 62304 §7.1.2-7.1.3 with 28 open issues as its
 * "published anomaly list". See implementation_plan_iio_afe.md §2.2 and §3.
 *
 * What it deliberately does NOT do: no ioctl or configfs ABI, no NVRAM
 * provisioning, no persistent chip settings, no generic interrupt emulation.
 * The scope is a SPI controller, a GPIO chip and the interrupt *counter*, and
 * every line beyond that is surface.
 *
 * The interrupt counter, and why it is a counter
 * ----------------------------------------------
 * USB HID has no asynchronous delivery to a host: a device cannot interrupt,
 * it can only answer when polled. So DRDY cannot become an IRQ here no matter
 * how the driver is written. What the bridge does offer is an edge counter on
 * GP6, and the difference between "edges the converter produced" and "samples
 * the host collected" is the only way this link can know it lost data. That
 * number is exported as a sysfs attribute rather than plumbed into the AFE
 * driver on purpose - keeping the two modules independent means neither has to
 * be loaded for the other to work, and the comparison belongs to whoever is
 * doing the measurement.
 *
 * PROTOCOL CONSTANTS ARE UNVERIFIED. Every offset, opcode and status byte
 * below marked [DS20005176?] was written from the MCP2210 protocol as
 * understood here and has NOT been checked against the datasheet or against
 * silicon - including the product ID. They are collected in one block so that
 * checking them is a single pass. Until that pass happens they are debt, in
 * the sense BRINGUP_STM32MP2.md §11 rule 1 gives the word.
 */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/hid.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>

#include <asm/unaligned.h>

#define MCP2210_USB_VENDOR_ID		0x04d8	/* Microchip */
#define MCP2210_USB_PRODUCT_ID		0x00de	/* [DS20005176?] */

/* Every exchange is one 64-byte report out and one 64-byte report in */
#define MCP2210_REPORT_SIZE		64
/* A single SPI Transfer command carries at most this many data bytes */
#define MCP2210_MAX_XFER_CHUNK		60
/* Nine general purpose pins, GP0..GP8; GP6 has the interrupt counter */
#define MCP2210_NGPIO			9
#define MCP2210_INTERRUPT_PIN		6

/* Command opcodes [DS20005176?] */
#define MCP2210_CMD_GET_CHIP_STATUS	0x10
#define MCP2210_CMD_SPI_CANCEL		0x11
#define MCP2210_CMD_GET_INT_COUNT	0x12
#define MCP2210_CMD_GET_CHIP_SETTINGS	0x20
#define MCP2210_CMD_SET_CHIP_SETTINGS	0x21
#define MCP2210_CMD_SET_GPIO_VALUE	0x30
#define MCP2210_CMD_GET_GPIO_VALUE	0x31
#define MCP2210_CMD_SET_GPIO_DIR	0x32
#define MCP2210_CMD_GET_GPIO_DIR	0x33
#define MCP2210_CMD_SET_SPI_SETTINGS	0x40
#define MCP2210_CMD_GET_SPI_SETTINGS	0x41
#define MCP2210_CMD_SPI_TRANSFER	0x42

/* Status byte, offset 1 of every response [DS20005176?] */
#define MCP2210_STATUS_OK		0x00
#define MCP2210_STATUS_BUS_UNAVAILABLE	0xf7
#define MCP2210_STATUS_BUSY		0xf8

/* SPI engine status, offset 3 of a transfer response [DS20005176?] */
#define MCP2210_SPI_STARTED_NO_DATA	0x10
#define MCP2210_SPI_NOT_FINISHED	0x20
#define MCP2210_SPI_FINISHED		0x30

/* Field offsets inside the 64-byte reports [DS20005176?] */
#define MCP2210_OFF_CMD			0
#define MCP2210_OFF_STATUS		1
#define MCP2210_OFF_XFER_LEN		1	/* in a SPI Transfer request */
#define MCP2210_OFF_RX_COUNT		2
#define MCP2210_OFF_ENGINE_STATUS	3
#define MCP2210_OFF_DATA		4
#define MCP2210_OFF_INT_COUNT		4	/* u16 LE */
#define MCP2210_OFF_GPIO_VALUE		4	/* u16 LE */
#define MCP2210_OFF_GPIO_DIR		4	/* u16 LE */
#define MCP2210_OFF_PIN_DESIGNATION	4	/* nine bytes, GP0..GP8 */
#define MCP2210_OFF_OTHER_SETTINGS	17

/* SPI Transfer Settings request layout [DS20005176?] */
#define MCP2210_OFF_BITRATE		4	/* u32 LE */
#define MCP2210_OFF_IDLE_CS		8	/* u16 LE */
#define MCP2210_OFF_ACTIVE_CS		10	/* u16 LE */
#define MCP2210_OFF_CS_TO_DATA		12	/* u16 LE, 100 us quanta */
#define MCP2210_OFF_DATA_TO_CS		14	/* u16 LE */
#define MCP2210_OFF_DATA_TO_DATA	16	/* u16 LE */
#define MCP2210_OFF_XFER_BYTES		18	/* u16 LE, whole transaction */
#define MCP2210_OFF_SPI_MODE		20

/* Pin designations [DS20005176?] */
#define MCP2210_PIN_GPIO		0x00
#define MCP2210_PIN_CHIP_SELECT		0x01
#define MCP2210_PIN_DEDICATED		0x02

/* Interrupt pin mode, bits 3:1 of the "other chip settings" byte */
#define MCP2210_MASK_INT_MODE		GENMASK(3, 1)
#define MCP2210_INT_MODE_NONE		0
#define MCP2210_INT_MODE_FALLING	1
#define MCP2210_INT_MODE_RISING		2

#define MCP2210_MAX_MESSAGE_BYTES	512
#define MCP2210_REPLY_TIMEOUT_MS	1000

/*
 * What is on the other side of the bridge.
 *
 * The MCP2210 and the converter are on the same board, so "which SPI device is
 * behind this bridge" is a board fact and this adjunct layer is the place that
 * is allowed to know it - the same argument that lets meta-med-bsp name a
 * partition. It is a module parameter and not a constant so that the seam
 * exists the day the answer stops being true, and so that a bench session can
 * bring the bridge up alone, with no device attached, to check enumeration and
 * the GPIO before any analogue hardware is involved: mcp2210_spi.spi_device=""
 */
static char *spi_device = "ads1299";
module_param(spi_device, charp, 0444);
MODULE_PARM_DESC(spi_device,
		 "modalias of the SPI device on the bridge, empty for none");

static int spi_chip_select;
module_param(spi_chip_select, int, 0444);
MODULE_PARM_DESC(spi_chip_select, "GP line used as chip select (0-8)");

static unsigned int spi_max_speed_hz = 4000000;
module_param(spi_max_speed_hz, uint, 0444);
MODULE_PARM_DESC(spi_max_speed_hz, "SPI clock for the attached device");

/*
 * SPI mode 1 (CPOL=0, CPHA=1) is what the ADS129x family needs. Left as a
 * parameter rather than hardcoded because the bridge itself has no opinion.
 */
static unsigned int spi_mode_param = SPI_MODE_1;
module_param(spi_mode_param, uint, 0444);
MODULE_PARM_DESC(spi_mode_param, "SPI mode 0-3 for the attached device");

struct mcp2210 {
	struct hid_device *hdev;
	struct spi_controller *ctlr;
	struct spi_device *child;
	struct gpio_chip gc;

	/* Serialises one request/response exchange over the HID endpoints */
	struct mutex lock;
	struct completion wait_in_report;

	u8 txbuf[MCP2210_REPORT_SIZE];
	u8 rxbuf[MCP2210_REPORT_SIZE];
	u8 status;

	/* Flattened message buffers, protected by lock */
	u8 tx_flat[MCP2210_MAX_MESSAGE_BYTES];
	u8 rx_flat[MCP2210_MAX_MESSAGE_BYTES];

	/* Cached chip settings, so a GPIO change does not clobber the CS pin */
	u8 pin_designation[MCP2210_NGPIO];
	u8 other_settings;
};

/* ------------------------------------------------------------------ report */

static int mcp2210_send_report(struct mcp2210 *mcp, size_t len)
{
	u8 *buf;
	int ret;

	buf = kmemdup(mcp->txbuf, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* The bridge uses an interrupt endpoint for output reports */
	ret = hid_hw_output_report(mcp->hdev, buf, len);
	kfree(buf);

	return ret < 0 ? ret : 0;
}

/*
 * One exchange. Caller holds mcp->lock and has filled txbuf; on success rxbuf
 * holds the whole 64-byte response and the device's status byte was OK.
 */
static int mcp2210_command(struct mcp2210 *mcp)
{
	unsigned long left;
	int ret;

	reinit_completion(&mcp->wait_in_report);

	ret = mcp2210_send_report(mcp, MCP2210_REPORT_SIZE);
	if (ret)
		return ret;

	left = wait_for_completion_timeout(&mcp->wait_in_report,
					   msecs_to_jiffies(MCP2210_REPLY_TIMEOUT_MS));
	if (!left)
		return -ETIMEDOUT;

	switch (mcp->status) {
	case MCP2210_STATUS_OK:
		return 0;
	case MCP2210_STATUS_BUSY:
		return -EBUSY;
	case MCP2210_STATUS_BUS_UNAVAILABLE:
		return -EBUSY;
	default:
		hid_dbg(mcp->hdev, "command 0x%02x returned status 0x%02x\n",
			mcp->txbuf[MCP2210_OFF_CMD], mcp->status);
		return -EIO;
	}
}

static void mcp2210_begin(struct mcp2210 *mcp, u8 command)
{
	memset(mcp->txbuf, 0, sizeof(mcp->txbuf));
	mcp->txbuf[MCP2210_OFF_CMD] = command;
}

static int mcp2210_raw_event(struct hid_device *hdev,
			     struct hid_report *report, u8 *data, int size)
{
	struct mcp2210 *mcp = hid_get_drvdata(hdev);

	if (!mcp || size < 2)
		return 0;

	memset(mcp->rxbuf, 0, sizeof(mcp->rxbuf));
	memcpy(mcp->rxbuf, data, min_t(int, size, MCP2210_REPORT_SIZE));
	mcp->status = mcp->rxbuf[MCP2210_OFF_STATUS];
	complete(&mcp->wait_in_report);

	return 1;
}

/* ------------------------------------------------------------- chip settings */

static int mcp2210_read_chip_settings(struct mcp2210 *mcp)
{
	int ret;

	mcp2210_begin(mcp, MCP2210_CMD_GET_CHIP_SETTINGS);
	ret = mcp2210_command(mcp);
	if (ret)
		return ret;

	memcpy(mcp->pin_designation, mcp->rxbuf + MCP2210_OFF_PIN_DESIGNATION,
	       MCP2210_NGPIO);
	mcp->other_settings = mcp->rxbuf[MCP2210_OFF_OTHER_SETTINGS];

	return 0;
}

/*
 * Rewrite the volatile chip settings from the cache. Read-modify-write and not
 * a blind write: the pin designations decide which pin is the chip select, and
 * a driver that reconstructed them from its own assumptions would silently
 * turn the CS line into a GPIO on any board wired differently from the one it
 * was written on.
 */
static int mcp2210_write_chip_settings(struct mcp2210 *mcp)
{
	mcp2210_begin(mcp, MCP2210_CMD_SET_CHIP_SETTINGS);
	memcpy(mcp->txbuf + MCP2210_OFF_PIN_DESIGNATION, mcp->pin_designation,
	       MCP2210_NGPIO);
	mcp->txbuf[MCP2210_OFF_OTHER_SETTINGS] = mcp->other_settings;

	return mcp2210_command(mcp);
}

/* ---------------------------------------------------------------------- spi */

static int mcp2210_set_transfer_settings(struct mcp2210 *mcp,
					 struct spi_device *spi,
					 u32 speed_hz, u16 total_bytes)
{
	u16 idle_cs = GENMASK(MCP2210_NGPIO - 1, 0);
	u16 active_cs = (u16)(idle_cs & ~BIT(spi_get_chipselect(spi, 0)));

	if (spi->mode & SPI_CS_HIGH)
		swap(idle_cs, active_cs);

	mcp2210_begin(mcp, MCP2210_CMD_SET_SPI_SETTINGS);
	put_unaligned_le32(speed_hz, mcp->txbuf + MCP2210_OFF_BITRATE);
	put_unaligned_le16(idle_cs, mcp->txbuf + MCP2210_OFF_IDLE_CS);
	put_unaligned_le16(active_cs, mcp->txbuf + MCP2210_OFF_ACTIVE_CS);
	put_unaligned_le16(0, mcp->txbuf + MCP2210_OFF_CS_TO_DATA);
	put_unaligned_le16(0, mcp->txbuf + MCP2210_OFF_DATA_TO_CS);
	put_unaligned_le16(0, mcp->txbuf + MCP2210_OFF_DATA_TO_DATA);
	put_unaligned_le16(total_bytes, mcp->txbuf + MCP2210_OFF_XFER_BYTES);
	mcp->txbuf[MCP2210_OFF_SPI_MODE] = spi->mode & (SPI_CPOL | SPI_CPHA);

	return mcp2210_command(mcp);
}

/*
 * Run one flattened transaction. The bridge holds chip select asserted for
 * exactly the number of bytes declared in the transfer settings, so a message
 * longer than one 60-byte chunk stays one SPI transaction as far as the
 * attached device is concerned - which is the property the ADS1299 needs,
 * since it has no FIFO and its 27-byte frame must leave before the next
 * conversion.
 */
static int mcp2210_do_transaction(struct mcp2210 *mcp, struct spi_device *spi,
				  u32 speed_hz, size_t len)
{
	size_t sent = 0;
	size_t received = 0;
	unsigned int stalls = 0;
	int ret;

	ret = mcp2210_set_transfer_settings(mcp, spi, speed_hz, (u16)len);
	if (ret)
		return ret;

	while (received < len) {
		size_t chunk = min(len - sent, (size_t)MCP2210_MAX_XFER_CHUNK);
		u8 engine;
		u8 got;

		mcp2210_begin(mcp, MCP2210_CMD_SPI_TRANSFER);
		mcp->txbuf[MCP2210_OFF_XFER_LEN] = (u8)chunk;
		if (chunk)
			memcpy(mcp->txbuf + MCP2210_OFF_DATA, mcp->tx_flat + sent,
			       chunk);

		ret = mcp2210_command(mcp);
		if (ret == -EBUSY) {
			/*
			 * The engine is still chewing on the previous chunk.
			 * Bounded, because an unbounded retry here would turn a
			 * wedged bridge into a hung acquisition thread rather
			 * than into an error the service can report.
			 */
			if (++stalls > 100)
				return -ETIMEDOUT;
			usleep_range(100, 200);
			continue;
		}
		if (ret)
			return ret;

		stalls = 0;
		sent += chunk;

		got = mcp->rxbuf[MCP2210_OFF_RX_COUNT];
		engine = mcp->rxbuf[MCP2210_OFF_ENGINE_STATUS];

		if (got) {
			if (received + got > len)
				got = (u8)(len - received);
			memcpy(mcp->rx_flat + received,
			       mcp->rxbuf + MCP2210_OFF_DATA, got);
			received += got;
		}

		if (engine == MCP2210_SPI_FINISHED && received >= len)
			break;

		if (sent == len && !got) {
			/* Nothing left to push; poll for the remaining bytes */
			if (++stalls > 100)
				return -ETIMEDOUT;
			usleep_range(100, 200);
		}
	}

	return 0;
}

static int mcp2210_transfer_one_message(struct spi_controller *ctlr,
					struct spi_message *msg)
{
	struct mcp2210 *mcp = spi_controller_get_devdata(ctlr);
	struct spi_device *spi = msg->spi;
	struct spi_transfer *xfer;
	size_t total = 0;
	size_t offset;
	u32 speed_hz = spi->max_speed_hz;
	int ret = 0;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		total += xfer->len;
		if (xfer->speed_hz && xfer->speed_hz < speed_hz)
			speed_hz = xfer->speed_hz;
	}

	if (total == 0)
		goto done;

	if (total > MCP2210_MAX_MESSAGE_BYTES) {
		ret = -EMSGSIZE;
		goto done;
	}

	mutex_lock(&mcp->lock);

	memset(mcp->tx_flat, 0, total);
	offset = 0;
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (xfer->tx_buf)
			memcpy(mcp->tx_flat + offset, xfer->tx_buf, xfer->len);
		offset += xfer->len;
	}

	ret = mcp2210_do_transaction(mcp, spi, speed_hz, total);
	if (!ret) {
		offset = 0;
		list_for_each_entry(xfer, &msg->transfers, transfer_list) {
			if (xfer->rx_buf)
				memcpy(xfer->rx_buf, mcp->rx_flat + offset,
				       xfer->len);
			offset += xfer->len;
			msg->actual_length += xfer->len;
		}
	}

	mutex_unlock(&mcp->lock);

done:
	/*
	 * Always zero. The core treats a non-zero return as "the message was
	 * never handed over" and finalises it itself
	 * (spi.c, __spi_pump_transfer_message), so returning ret here after
	 * having finalised would complete the same message twice; the failure
	 * is reported through msg->status, which is where the caller reads it.
	 */
	msg->status = ret;
	spi_finalize_current_message(ctlr);

	return 0;
}

/* --------------------------------------------------------------------- gpio */

static int mcp2210_gpio_get_value(struct mcp2210 *mcp, u16 *value)
{
	int ret;

	mcp2210_begin(mcp, MCP2210_CMD_GET_GPIO_VALUE);
	ret = mcp2210_command(mcp);
	if (ret)
		return ret;

	*value = get_unaligned_le16(mcp->rxbuf + MCP2210_OFF_GPIO_VALUE);

	return 0;
}

static int mcp2210_gpio_get_dir_raw(struct mcp2210 *mcp, u16 *dir)
{
	int ret;

	mcp2210_begin(mcp, MCP2210_CMD_GET_GPIO_DIR);
	ret = mcp2210_command(mcp);
	if (ret)
		return ret;

	*dir = get_unaligned_le16(mcp->rxbuf + MCP2210_OFF_GPIO_DIR);

	return 0;
}

static int mcp2210_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct mcp2210 *mcp = gpiochip_get_data(gc);
	u16 value;
	int ret;

	mutex_lock(&mcp->lock);
	ret = mcp2210_gpio_get_value(mcp, &value);
	mutex_unlock(&mcp->lock);

	if (ret)
		return ret;

	return !!(value & BIT(offset));
}

static void mcp2210_gpio_set(struct gpio_chip *gc, unsigned int offset,
			     int state)
{
	struct mcp2210 *mcp = gpiochip_get_data(gc);
	u16 value;

	mutex_lock(&mcp->lock);

	if (mcp2210_gpio_get_value(mcp, &value))
		goto out;

	if (state)
		value |= BIT(offset);
	else
		value &= (u16)~BIT(offset);

	mcp2210_begin(mcp, MCP2210_CMD_SET_GPIO_VALUE);
	put_unaligned_le16(value, mcp->txbuf + MCP2210_OFF_GPIO_VALUE);
	mcp2210_command(mcp);

out:
	mutex_unlock(&mcp->lock);
}

static int mcp2210_gpio_set_dir(struct mcp2210 *mcp, unsigned int offset,
				bool input)
{
	u16 dir;
	int ret;

	ret = mcp2210_gpio_get_dir_raw(mcp, &dir);
	if (ret)
		return ret;

	if (input)
		dir |= BIT(offset);
	else
		dir &= (u16)~BIT(offset);

	mcp2210_begin(mcp, MCP2210_CMD_SET_GPIO_DIR);
	put_unaligned_le16(dir, mcp->txbuf + MCP2210_OFF_GPIO_DIR);

	return mcp2210_command(mcp);
}

static int mcp2210_gpio_direction_input(struct gpio_chip *gc,
					unsigned int offset)
{
	struct mcp2210 *mcp = gpiochip_get_data(gc);
	int ret;

	mutex_lock(&mcp->lock);
	ret = mcp2210_gpio_set_dir(mcp, offset, true);
	mutex_unlock(&mcp->lock);

	return ret;
}

static int mcp2210_gpio_direction_output(struct gpio_chip *gc,
					 unsigned int offset, int value)
{
	struct mcp2210 *mcp = gpiochip_get_data(gc);
	int ret;

	mcp2210_gpio_set(gc, offset, value);

	mutex_lock(&mcp->lock);
	ret = mcp2210_gpio_set_dir(mcp, offset, false);
	mutex_unlock(&mcp->lock);

	return ret;
}

static int mcp2210_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct mcp2210 *mcp = gpiochip_get_data(gc);
	u16 dir;
	int ret;

	mutex_lock(&mcp->lock);
	ret = mcp2210_gpio_get_dir_raw(mcp, &dir);
	mutex_unlock(&mcp->lock);

	if (ret)
		return ret;

	return (dir & BIT(offset)) ? GPIO_LINE_DIRECTION_IN
				   : GPIO_LINE_DIRECTION_OUT;
}

/*
 * A pin the chip settings designate as chip select or as a dedicated function
 * is not ours to drive. Refusing here rather than letting a request through is
 * the difference between a configuration error reported at request time and an
 * acquisition that stops mid-recording because something toggled CS.
 */
static int mcp2210_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct mcp2210 *mcp = gpiochip_get_data(gc);

	if (mcp->pin_designation[offset] != MCP2210_PIN_GPIO)
		return -EBUSY;

	return 0;
}

/* ---------------------------------------------------------------- interrupt */

static ssize_t interrupt_count_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct mcp2210 *mcp = hid_get_drvdata(hdev);
	u16 count;
	int ret;

	mutex_lock(&mcp->lock);
	mcp2210_begin(mcp, MCP2210_CMD_GET_INT_COUNT);
	/*
	 * Do not reset the counter on read. A reader that consumed the count
	 * would make the number unusable to any second reader, and the only
	 * thing this value is for is being compared against a sample count
	 * held somewhere else.
	 */
	mcp->txbuf[1] = 0;
	ret = mcp2210_command(mcp);
	if (!ret)
		count = get_unaligned_le16(mcp->rxbuf + MCP2210_OFF_INT_COUNT);
	mutex_unlock(&mcp->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", count);
}
static DEVICE_ATTR_RO(interrupt_count);

static ssize_t interrupt_count_reset_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t len)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct mcp2210 *mcp = hid_get_drvdata(hdev);
	int ret;

	mutex_lock(&mcp->lock);
	mcp2210_begin(mcp, MCP2210_CMD_GET_INT_COUNT);
	mcp->txbuf[1] = 1;
	ret = mcp2210_command(mcp);
	mutex_unlock(&mcp->lock);

	return ret ? ret : (ssize_t)len;
}
static DEVICE_ATTR_WO(interrupt_count_reset);

static struct attribute *mcp2210_attrs[] = {
	&dev_attr_interrupt_count.attr,
	&dev_attr_interrupt_count_reset.attr,
	NULL
};
ATTRIBUTE_GROUPS(mcp2210);

/*
 * Ask the bridge to count DRDY edges on GP6. Falling, because the ADS129x
 * family asserts DRDY low when a conversion is ready.
 */
static int mcp2210_enable_interrupt_counter(struct mcp2210 *mcp)
{
	int ret;

	ret = mcp2210_read_chip_settings(mcp);
	if (ret)
		return ret;

	if (mcp->pin_designation[MCP2210_INTERRUPT_PIN] != MCP2210_PIN_DEDICATED) {
		hid_warn(mcp->hdev,
			 "GP%u is not the dedicated interrupt pin; sample loss cannot be counted\n",
			 MCP2210_INTERRUPT_PIN);
		return 0;
	}

	mcp->other_settings &= (u8)~MCP2210_MASK_INT_MODE;
	mcp->other_settings |= FIELD_PREP(MCP2210_MASK_INT_MODE,
					  MCP2210_INT_MODE_FALLING);

	return mcp2210_write_chip_settings(mcp);
}

/* -------------------------------------------------------------------- probe */

static void mcp2210_hid_stop(void *data)
{
	struct hid_device *hdev = data;

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static void mcp2210_unregister_child(void *data)
{
	struct spi_device *child = data;

	spi_unregister_device(child);
}

static int mcp2210_register_child(struct mcp2210 *mcp)
{
	struct spi_board_info info = { };

	if (!spi_device || !*spi_device)
		return 0;

	if (spi_chip_select < 0 || spi_chip_select >= MCP2210_NGPIO)
		return -EINVAL;

	strscpy(info.modalias, spi_device, sizeof(info.modalias));
	info.max_speed_hz = spi_max_speed_hz;
	info.chip_select = (u8)spi_chip_select;
	info.mode = spi_mode_param & (SPI_CPOL | SPI_CPHA | SPI_CS_HIGH);
	/*
	 * irq stays 0 on purpose, and it is load bearing: it is how the AFE
	 * driver learns that DRDY cannot reach it and that its timestamps are
	 * therefore host side. See ti-ads1299.c, ts_source.
	 */
	info.irq = 0;

	mcp->child = spi_new_device(mcp->ctlr, &info);
	if (!mcp->child)
		return -ENODEV;

	return devm_add_action_or_reset(&mcp->hdev->dev,
					mcp2210_unregister_child, mcp->child);
}

static int mcp2210_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct spi_controller *ctlr;
	struct mcp2210 *mcp;
	int ret;

	ctlr = devm_spi_alloc_host(&hdev->dev, sizeof(*mcp));
	if (!ctlr)
		return -ENOMEM;

	mcp = spi_controller_get_devdata(ctlr);
	mcp->ctlr = ctlr;
	mcp->hdev = hdev;
	mutex_init(&mcp->lock);
	init_completion(&mcp->wait_in_report);
	hid_set_drvdata(hdev, mcp);

	ret = hid_parse(hdev);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "cannot parse reports\n");

	/*
	 * No HID_CONNECT_* flags: this driver consumes raw reports and must not
	 * also expose the bridge as an input device. Winning the bind against
	 * hid-generic needs nothing more than this id_table - verified in the
	 * 6.6 source rather than assumed, because the plan flagged it as a risk
	 * and the third-party project calls it painful: hid_generic_match()
	 * walks every registered HID driver and declines any device another
	 * driver matches (drivers/hid/hid-generic.c:37-57). What that does
	 * require is that this module be loaded *before* the device enumerates,
	 * which is why the recipe puts it in KERNEL_MODULE_AUTOLOAD.
	 */
	ret = hid_hw_start(hdev, 0);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "cannot start hardware\n");

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_hw_stop(hdev);
		return dev_err_probe(&hdev->dev, ret, "cannot open device\n");
	}

	ret = devm_add_action_or_reset(&hdev->dev, mcp2210_hid_stop, hdev);
	if (ret)
		return ret;

	hid_device_io_start(hdev);

	ret = mcp2210_enable_interrupt_counter(mcp);
	if (ret)
		return dev_err_probe(&hdev->dev, ret,
				     "cannot read or set chip settings\n");

	ctlr->bus_num = -1;
	ctlr->num_chipselect = MCP2210_NGPIO;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->min_speed_hz = 1500;
	ctlr->max_speed_hz = 12000000;
	ctlr->transfer_one_message = mcp2210_transfer_one_message;
	ctlr->dev.parent = &hdev->dev;

	ret = devm_spi_register_controller(&hdev->dev, ctlr);
	if (ret)
		return dev_err_probe(&hdev->dev, ret,
				     "cannot register SPI controller\n");

#if IS_REACHABLE(CONFIG_GPIOLIB)
	mcp->gc.label = "mcp2210";
	mcp->gc.parent = &hdev->dev;
	mcp->gc.owner = THIS_MODULE;
	mcp->gc.base = -1;
	mcp->gc.ngpio = MCP2210_NGPIO;
	mcp->gc.can_sleep = true;
	mcp->gc.request = mcp2210_gpio_request;
	mcp->gc.get = mcp2210_gpio_get;
	mcp->gc.set = mcp2210_gpio_set;
	mcp->gc.get_direction = mcp2210_gpio_get_direction;
	mcp->gc.direction_input = mcp2210_gpio_direction_input;
	mcp->gc.direction_output = mcp2210_gpio_direction_output;

	ret = devm_gpiochip_add_data(&hdev->dev, &mcp->gc, mcp);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "cannot add gpiochip\n");
#endif

	ret = mcp2210_register_child(mcp);
	if (ret)
		return dev_err_probe(&hdev->dev, ret,
				     "cannot attach '%s' to the bridge\n",
				     spi_device);

	hid_info(hdev, "USB-SPI bridge ready, %u chip selects, %s attached\n",
		 MCP2210_NGPIO,
		 (spi_device && *spi_device) ? spi_device : "nothing");

	return 0;
}

static const struct hid_device_id mcp2210_devices[] = {
	{ HID_USB_DEVICE(MCP2210_USB_VENDOR_ID, MCP2210_USB_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, mcp2210_devices);

static struct hid_driver mcp2210_driver = {
	.name		= "mcp2210",
	.id_table	= mcp2210_devices,
	.probe		= mcp2210_probe,
	.raw_event	= mcp2210_raw_event,
	.driver = {
		.dev_groups = mcp2210_groups,
	},
};
module_hid_driver(mcp2210_driver);

MODULE_AUTHOR("MedPlatform (TCC)");
MODULE_DESCRIPTION("MCP2210 USB to SPI bridge, as a spi_controller");
MODULE_LICENSE("GPL");
