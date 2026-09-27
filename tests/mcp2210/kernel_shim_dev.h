/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The driver model, HID, SPI, GPIO and sysfs surfaces that hid-mcp2210.c
 * touches. See kernel_shim.h for why this exists.
 *
 * The one seam that matters is hid_hw_output_report(): it hands the outgoing
 * 64-byte report to the fake device, which may answer by calling the driver's
 * own .raw_event - the same path the HID core uses for an interrupt IN report.
 * Everything the driver does with that answer, including mcp2210_command()'s
 * status decoding, is therefore under test rather than replaced.
 */

#ifndef MCP2210_TEST_KERNEL_SHIM_DEV_H
#define MCP2210_TEST_KERNEL_SHIM_DEV_H

#include "kernel_shim.h"

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/* ------------------------------------------------------------------ list */

struct list_head { struct list_head *next, *prev; };

static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static inline void list_add_tail(struct list_head *n, struct list_head *head)
{
	n->prev = head->prev;
	n->next = head;
	head->prev->next = n;
	head->prev = n;
}
#define list_entry(p, type, member)	container_of(p, type, member)
#define list_for_each_entry(pos, head, member)				\
	for (pos = list_entry((head)->next, typeof(*pos), member);	\
	     &pos->member != (head);					\
	     pos = list_entry(pos->member.next, typeof(*pos), member))

/* --------------------------------------------------------- log capture */

#define SHIM_LOG_LINES	64
#define SHIM_LOG_LEN	256
extern char shim_log[SHIM_LOG_LINES][SHIM_LOG_LEN];
extern unsigned int shim_log_count;
void shim_log_reset(void);
bool shim_log_contains(const char *needle);

void shim_logf(const char *level, const char *fmt, ...);

#define hid_info(hdev, fmt, ...)  shim_logf("info", fmt, ##__VA_ARGS__)
#define hid_warn(hdev, fmt, ...)  shim_logf("warn", fmt, ##__VA_ARGS__)
#define hid_err(hdev, fmt, ...)   shim_logf("err", fmt, ##__VA_ARGS__)
#define hid_dbg(hdev, fmt, ...)   shim_logf("dbg", fmt, ##__VA_ARGS__)

/* --------------------------------------------------------- driver model */

#define SHIM_MAX_ACTIONS	16

struct device {
	void (*action_fn[SHIM_MAX_ACTIONS])(void *);
	void *action_data[SHIM_MAX_ACTIONS];
	unsigned int actions;
	void *parent;
};

static inline int devm_add_action_or_reset(struct device *dev,
					   void (*fn)(void *), void *data)
{
	if (dev->actions >= SHIM_MAX_ACTIONS) {
		fn(data);
		return -ENOMEM;
	}
	dev->action_fn[dev->actions] = fn;
	dev->action_data[dev->actions] = data;
	dev->actions++;
	return 0;
}

/* Unwind in reverse, the way devres does, so a test can exercise removal. */
static inline void shim_devm_release(struct device *dev)
{
	while (dev->actions) {
		dev->actions--;
		dev->action_fn[dev->actions](dev->action_data[dev->actions]);
	}
}

#define dev_err_probe(dev, err, fmt, ...)			\
	({ shim_logf("err", fmt, ##__VA_ARGS__); (err); })

/* ------------------------------------------------------------------- hid */

struct hid_device_id { u32 bus; u32 vendor; u32 product; };
struct hid_report;

struct hid_device {
	struct device dev;
	void *driver_data;
	u32 version;
	const char *name;
	const char *phys;
};

struct hid_driver {
	const char *name;
	const struct hid_device_id *id_table;
	int (*probe)(struct hid_device *, const struct hid_device_id *);
	void (*remove)(struct hid_device *);
	int (*raw_event)(struct hid_device *, struct hid_report *, u8 *, int);
	struct { const struct attribute_group **dev_groups; } driver;
};

#define BUS_USB	0x03
#define HID_USB_DEVICE(v, p)	.bus = BUS_USB, .vendor = (v), .product = (p)
#define MODULE_DEVICE_TABLE(t, n)

static inline void hid_set_drvdata(struct hid_device *h, void *d) { h->driver_data = d; }
static inline void *hid_get_drvdata(struct hid_device *h) { return h->driver_data; }
#define to_hid_device(d)	container_of(d, struct hid_device, dev)

extern int shim_hid_parse_ret, shim_hid_start_ret, shim_hid_open_ret;
static inline int hid_parse(struct hid_device *h) { (void)h; return shim_hid_parse_ret; }
static inline int hid_hw_start(struct hid_device *h, unsigned f) { (void)h; (void)f; return shim_hid_start_ret; }
static inline int hid_hw_open(struct hid_device *h) { (void)h; return shim_hid_open_ret; }
static inline void hid_hw_close(struct hid_device *h) { (void)h; }
static inline void hid_hw_stop(struct hid_device *h) { (void)h; }
static inline void hid_device_io_start(struct hid_device *h) { (void)h; }

/* The seam. Implemented by the fake device; see fake_mcp2210.h. */
int hid_hw_output_report(struct hid_device *hdev, u8 *buf, size_t len);

/* Filled in by module_hid_driver() at the bottom of the driver. */
struct hid_driver *shim_driver_ref(void);
#define module_hid_driver(drv) \
	struct hid_driver *shim_driver_ref(void) { return &(drv); }

#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_PARM_DESC(a, b)
#define module_param(name, type, perm)

/* ----------------------------------------------------------------- sysfs */

struct attribute { const char *name; };
struct attribute_group { struct attribute **attrs; };
struct device_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *, struct device_attribute *, char *);
	ssize_t (*store)(struct device *, struct device_attribute *, const char *, size_t);
};

#define __ATTR_RO(_name) { .attr = { .name = #_name }, .show = _name##_show }
#define __ATTR_WO(_name) { .attr = { .name = #_name }, .store = _name##_store }
/* No `static` here: the kernel's macros do not add it and the driver does. */
#define DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_WO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#define ATTRIBUTE_GROUPS(_name)						\
	static const struct attribute_group _name##_group = {		\
		.attrs = _name##_attrs,					\
	};								\
	static const struct attribute_group *_name##_groups[] __maybe_unused = { \
		&_name##_group, NULL,					\
	}

#define sysfs_emit(buf, fmt, ...)	sprintf(buf, fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------- spi */

#define SPI_CPHA	BIT(0)
#define SPI_CPOL	BIT(1)
#define SPI_MODE_0	0
#define SPI_MODE_1	SPI_CPHA
#define SPI_MODE_2	SPI_CPOL
#define SPI_MODE_3	(SPI_CPOL | SPI_CPHA)
#define SPI_CS_HIGH	BIT(2)
#define SPI_BPW_MASK(bits)	BIT((bits) - 1)
#define SPI_NAME_SIZE	32

struct spi_controller;

struct spi_device {
	struct spi_controller *controller;
	u32 max_speed_hz;
	u32 mode;
	u8 chip_select;
	int irq;
	char modalias[SPI_NAME_SIZE];
};

struct spi_transfer {
	const void *tx_buf;
	void *rx_buf;
	unsigned int len;
	u32 speed_hz;
	unsigned int cs_change;
	struct { u16 value; u8 unit; } delay;
	struct list_head transfer_list;
};

struct spi_message {
	struct list_head transfers;
	struct spi_device *spi;
	unsigned int actual_length;
	int status;
};

struct spi_controller {
	struct device dev;
	int bus_num;
	u16 num_chipselect;
	u32 mode_bits;
	u32 bits_per_word_mask;
	u32 min_speed_hz;
	u32 max_speed_hz;
	int (*transfer_one_message)(struct spi_controller *, struct spi_message *);
	void *devdata;
	bool registered;
	bool finalized;
};

struct spi_board_info {
	char modalias[SPI_NAME_SIZE];
	const void *platform_data;
	const void *swnode;
	void *controller_data;
	int irq;
	u32 max_speed_hz;
	u16 bus_num;
	u16 chip_select;
	u32 mode;
};

static inline u8 spi_get_chipselect(struct spi_device *spi, u8 idx)
{
	(void)idx;
	return spi->chip_select;
}
static inline void *spi_controller_get_devdata(struct spi_controller *c) { return c->devdata; }
static inline void spi_finalize_current_message(struct spi_controller *c) { c->finalized = true; }

static inline struct spi_controller *devm_spi_alloc_host(struct device *dev, unsigned int size)
{
	struct spi_controller *c = kzalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return NULL;
	c->devdata = kzalloc(size, GFP_KERNEL);
	if (!c->devdata) {
		kfree(c);
		return NULL;
	}
	c->dev.parent = dev;
	return c;
}
static inline int devm_spi_register_controller(struct device *dev, struct spi_controller *c)
{
	(void)dev;
	c->registered = true;
	return 0;
}
static inline struct spi_device *spi_new_device(struct spi_controller *c,
						struct spi_board_info *info)
{
	struct spi_device *s = kzalloc(sizeof(*s), GFP_KERNEL);

	if (!s)
		return NULL;
	s->controller = c;
	s->max_speed_hz = info->max_speed_hz;
	s->mode = info->mode;
	s->chip_select = (u8)info->chip_select;
	s->irq = info->irq;
	strscpy(s->modalias, info->modalias, sizeof(s->modalias));
	return s;
}
static inline void spi_unregister_device(struct spi_device *s) { kfree(s); }

/* ------------------------------------------------------------------ gpio */

#define GPIO_LINE_DIRECTION_OUT	0
#define GPIO_LINE_DIRECTION_IN	1

struct gpio_chip {
	const char *label;
	struct device *parent;
	void *owner;
	int base;
	u16 ngpio;
	bool can_sleep;
	int (*request)(struct gpio_chip *, unsigned int);
	int (*get)(struct gpio_chip *, unsigned int);
	void (*set)(struct gpio_chip *, unsigned int, int);
	int (*get_direction)(struct gpio_chip *, unsigned int);
	int (*direction_input)(struct gpio_chip *, unsigned int);
	int (*direction_output)(struct gpio_chip *, unsigned int, int);
	void *shim_data;
};

static inline void *gpiochip_get_data(struct gpio_chip *gc) { return gc->shim_data; }
static inline int devm_gpiochip_add_data(struct device *dev, struct gpio_chip *gc, void *data)
{
	(void)dev;
	gc->shim_data = data;
	return 0;
}

#endif /* MCP2210_TEST_KERNEL_SHIM_DEV_H */
