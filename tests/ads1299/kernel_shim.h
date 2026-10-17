/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The subset of the kernel that ti-ads1299.c touches, reimplemented for the
 * host (docs/implementation_plan_usb_parity.md, phase 0.a).
 *
 * The same idea as tests/mcp2210: the driver is compiled BYTE FOR BYTE as it
 * ships, with no test macro and no seam of its own, and the substitution
 * happens one level below it - here, in the SPI core. Every transfer the
 * driver makes lands in fake_ads1299.c, a register-level model of the
 * converter, and comes back the way the part would answer it.
 *
 * What is faithful and what is not:
 *
 *  - Faithful: the SPI byte protocol (command, register read and write,
 *    RDATA with its status word), the regmap cache - which is what decides
 *    whether a read reaches the part - and the order in which the driver's
 *    callbacks run. A transfer started with spi_async() is held until the
 *    test completes it, so "a sample became due while one was in flight" is
 *    something a test can arrange rather than hope for.
 *  - Not faithful: there is no concurrency, no interrupt context, no real
 *    time and no allocation failure. A spinlock here is nothing. Anything
 *    this suite would say about locking or races is worth nothing.
 */

#ifndef ADS1299_TEST_KERNEL_SHIM_H
#define ADS1299_TEST_KERNEL_SHIM_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef unsigned short umode_t;
typedef s64 ktime_t;
typedef unsigned int gfp_t;

#define __aligned(x)		__attribute__((aligned(x)))
#define __packed		__attribute__((packed))
#define __maybe_unused		__attribute__((unused))
#define __always_unused		__attribute__((unused))

#define BIT(n)			(1UL << (n))
#define BIT_ULL(n)		(1ULL << (n))
#define GENMASK(h, l)		(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (64 - 1 - (h))))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define ALIGN(x, a)		(((x) + (a) - 1) / (a) * (a))
#define container_of(p, t, m)	((t *)((char *)(p) - offsetof(t, m)))

/* Type-generic, like the kernel's, so that an s64 is not narrowed to int. */
#define min(a, b)		({ typeof(a) _a = (a); typeof(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b)		({ typeof(a) _a = (a); typeof(b) _b = (b); _a > _b ? _a : _b; })
#undef abs
#define abs(x)			({ typeof(x) _x = (x); _x < 0 ? -_x : _x; })

#define __ffs(x)		((unsigned int)__builtin_ctzl(x))
#define FIELD_PREP(mask, val)	((((unsigned long)(val)) << __ffs(mask)) & (mask))
#define FIELD_GET(mask, val)	((((unsigned long)(val)) & (mask)) >> __ffs(mask))
#define FIELD_PREP_CONST(mask, val)	FIELD_PREP(mask, val)

#define U64_MAX			UINT64_MAX
#define S32_MAX			INT32_MAX
#define S32_MIN			INT32_MIN
#define MICRO			1000000UL
#define MICROHZ_PER_HZ		1000000UL
#define NSEC_PER_SEC		1000000000L

#define IS_ERR(p)		((unsigned long)(p) >= (unsigned long)-4095)
#define PTR_ERR(p)		((long)(p))
#define ERR_PTR(e)		((void *)(long)(e))

static inline s64 div_s64(s64 a, s32 b) { return a / b; }
static inline u64 div_u64(u64 a, u32 b) { return a / b; }
static inline int ilog2(unsigned long v) { return 63 - __builtin_clzl(v); }
static inline bool is_power_of_2(unsigned long v) { return v && !(v & (v - 1)); }
static inline s32 sign_extend32(u32 v, int index)
{
	int shift = 31 - index;

	return (s32)(v << shift) >> shift;
}
static inline u32 get_unaligned_be24(const void *p)
{
	const u8 *b = p;

	return ((u32)b[0] << 16) | ((u32)b[1] << 8) | b[2];
}

static inline bool test_bit(unsigned int nr, const unsigned long *addr)
{
	return (addr[nr / 64] >> (nr % 64)) & 1;
}
#define for_each_set_bit(bit, addr, size) \
	for ((bit) = 0; (bit) < (int)(size); (bit)++) \
		if (test_bit((unsigned int)(bit), (addr)))

/* cleanup.h: a guard is the lock it names, and a lock is nothing here. */
#define guard(type)		(void)
typedef int spinlock_t;
#define spin_lock_init(l)	(*(l) = 0)

/* ---------------------------------------------------------- module glue */

#define THIS_MODULE		NULL
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_DEVICE_TABLE(type, name)

/* ------------------------------------------------------------- devices */

struct device {
	const char *name;
};
static inline const char *dev_name(const struct device *d) { return d->name; }

extern int shim_verbose;
extern char shim_last_error[256];
extern char shim_error_log[2048];	/* every error since shim_reset() */
void shim_log(bool error, const char *fmt, ...);
#define dev_err(dev, ...)	((void)(dev), shim_log(true, __VA_ARGS__))
#define dev_info(dev, ...)	((void)(dev), shim_log(false, __VA_ARGS__))
#define dev_warn(dev, ...)	shim_log(false, __VA_ARGS__)
#define dev_dbg(dev, ...)	do { } while (0)
#define dev_err_probe(dev, err, ...) (shim_log(true, __VA_ARGS__), (int)(err))

#define GFP_KERNEL		0
static inline void *devm_kcalloc(struct device *d, size_t n, size_t s, gfp_t g)
{
	(void)d; (void)g;
	return calloc(n, s);
}
static inline void *devm_kzalloc(struct device *d, size_t s, gfp_t g)
{
	(void)d; (void)g;
	return calloc(1, s);
}
static inline void *kcalloc(size_t n, size_t s, gfp_t g) { (void)g; return calloc(n, s); }
static inline void kfree(const void *p) { free((void *)p); }
char *devm_kasprintf(struct device *d, gfp_t g, const char *fmt, ...);

/*
 * Firmware properties: the few the driver reads, set by the test. A property
 * is absent unless the test says otherwise.
 */
extern bool shim_prop_clock_cells;	/* "#clock-cells" */
extern u32 shim_prop_daisy;		/* "#daisy-chained-devices", 0: absent */
extern const char *shim_prop_clock_name; /* "clock-output-names", NULL: absent */
bool device_property_present(const struct device *d, const char *name);
int device_property_read_u32(const struct device *d, const char *name, u32 *val);
int device_property_read_string(const struct device *d, const char *name,
				const char **val);
int devm_add_action_or_reset(struct device *d, void (*fn)(void *), void *data);

/* ------------------------------------------------------- time, sleeping */

/* Time is a counter the shim advances; nothing sleeps. */
extern unsigned long shim_sleeps;
static inline void fsleep(unsigned long us) { (void)us; shim_sleeps++; }
static inline void msleep(unsigned int ms) { (void)ms; shim_sleeps++; }
static inline unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static inline ktime_t ns_to_ktime(u64 ns) { return (ktime_t)ns; }

struct completion { int done; };
static inline void init_completion(struct completion *c) { c->done = 0; }
static inline void reinit_completion(struct completion *c) { c->done = 0; }
static inline void complete(struct completion *c) { c->done = 1; }
static inline unsigned long wait_for_completion_timeout(struct completion *c, unsigned long t)
{
	(void)t;
	return c->done ? 1 : 0;
}

#define CLOCK_MONOTONIC		1
enum hrtimer_mode { HRTIMER_MODE_REL = 1 };
enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
struct hrtimer {
	enum hrtimer_restart (*function)(struct hrtimer *);
	bool active;
	ktime_t period;
};
static inline void hrtimer_init(struct hrtimer *t, int clock, enum hrtimer_mode m)
{
	(void)clock; (void)m;
	t->active = false;
}
static inline void hrtimer_start(struct hrtimer *t, ktime_t p, enum hrtimer_mode m)
{
	(void)m;
	t->active = true;
	t->period = p;
}
static inline int hrtimer_cancel(struct hrtimer *t) { bool was = t->active; t->active = false; return was; }
static inline u64 hrtimer_forward_now(struct hrtimer *t, ktime_t p) { t->period = p; return 1; }

/* ------------------------------------------------------ regulators, clk */

struct regulator { int uv; int enabled; };
struct clk { unsigned long rate; };
struct gpio_desc { int value; };
#define GPIOD_OUT_HIGH		1

extern struct regulator *shim_vref;	/* NULL: the part's internal reference */
extern struct regulator *shim_biasref;	/* NULL: BIASREF made inside the part */
extern struct regulator shim_avdd;
static inline struct regulator *devm_regulator_get_optional(struct device *d, const char *id)
{
	(void)d;
	if (!strcmp(id, "vref") && shim_vref)
		return shim_vref;
	if (!strcmp(id, "biasref") && shim_biasref)
		return shim_biasref;
	return ERR_PTR(-ENODEV);
}
static inline struct regulator *devm_regulator_get(struct device *d, const char *id)
{
	(void)d; (void)id;
	return &shim_avdd;
}
static inline int regulator_enable(struct regulator *r) { r->enabled++; return 0; }
static inline int regulator_disable(struct regulator *r) { r->enabled--; return 0; }
static inline int regulator_get_voltage(struct regulator *r) { return r->uv; }
/* An external clock on CLK when the test gives one, none otherwise. */
extern struct clk *shim_ext_clk;
static inline struct clk *devm_clk_get_optional_enabled(struct device *d, const char *id)
{
	(void)d; (void)id;
	return shim_ext_clk;
}

/* clk-provider.h: a provider registers one clk_hw, which the test drives. */
struct clk_hw;
struct clk_ops {
	unsigned long (*recalc_rate)(struct clk_hw *hw, unsigned long parent_rate);
	int (*prepare)(struct clk_hw *hw);
	void (*unprepare)(struct clk_hw *hw);
	int (*is_prepared)(struct clk_hw *hw);
};
struct clk_init_data {
	const char *name;
	const struct clk_ops *ops;
};
struct clk_hw {
	const struct clk_init_data *init;
};
extern struct clk_hw *shim_clk_hw;		/* the registered output, or NULL */
extern char shim_clk_hw_name[64];		/* its name, copied at registration */
extern const struct clk_ops *shim_clk_hw_ops;
int devm_clk_hw_register(struct device *d, struct clk_hw *hw);
struct clk_hw *of_clk_hw_simple_get(void *spec, void *data);
int devm_of_clk_add_hw_provider(struct device *d,
				struct clk_hw *(*get)(void *, void *), void *data);
static inline unsigned long clk_get_rate(struct clk *c) { return c ? c->rate : 0; }
static inline struct gpio_desc *devm_gpiod_get_optional(struct device *d, const char *id, int f)
{
	(void)d; (void)id; (void)f;
	return NULL;
}
static inline void gpiod_set_value_cansleep(struct gpio_desc *g, int v) { if (g) g->value = v; }

/* ---------------------------------------------------------- interrupts */

typedef enum { IRQ_NONE, IRQ_HANDLED } irqreturn_t;
#define IRQF_TRIGGER_FALLING	2
typedef irqreturn_t (*irq_handler_t)(int, void *);
extern irq_handler_t shim_irq_handler;
extern void *shim_irq_dev;
static inline int devm_request_irq(struct device *d, unsigned int irq, irq_handler_t h,
				   unsigned long flags, const char *name, void *dev_id)
{
	(void)d; (void)irq; (void)flags; (void)name;
	shim_irq_handler = h;
	shim_irq_dev = dev_id;
	return 0;
}

/* ----------------------------------------------------------------- SPI */

#define SPI_DELAY_UNIT_USECS	0
struct spi_delay { u16 value; u8 unit; };
struct spi_transfer {
	const void *tx_buf;
	void *rx_buf;
	unsigned int len;
	u32 speed_hz;
	struct spi_delay delay;
};
struct spi_message {
	struct spi_transfer *xfers;
	unsigned int n;
	void (*complete)(void *context);
	void *context;
};
struct spi_device {
	struct device dev;
	int irq;
	bool rt;		/* asks the SPI core for a real-time pump */
};
/* Records the call and what the device asked for; the bus is the fake's. */
int spi_setup(struct spi_device *spi);
struct spi_device_id { char name[32]; };
struct of_device_id { char compatible[128]; };
struct spi_driver {
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
	int (*probe)(struct spi_device *spi);
	const struct spi_device_id *id_table;
};
extern struct spi_driver *shim_spi_driver;
#define module_spi_driver(d)	struct spi_driver *shim_spi_driver = &(d)

static inline void spi_message_init_with_transfers(struct spi_message *m,
						   struct spi_transfer *x, unsigned int n)
{
	memset(m, 0, sizeof(*m));
	m->xfers = x;
	m->n = n;
}
/* The bus: every transfer goes to the fake converter (fake_ads1299.c). */
int spi_sync_transfer(struct spi_device *spi, struct spi_transfer *x, unsigned int n);
int spi_sync(struct spi_device *spi, struct spi_message *m);
int spi_async(struct spi_device *spi, struct spi_message *m);

/* -------------------------------------------------------------- regmap */

enum regcache_type { REGCACHE_NONE, REGCACHE_RBTREE, REGCACHE_FLAT, REGCACHE_MAPLE };
struct regmap_range { unsigned int range_min, range_max; };
#define regmap_reg_range(lo, hi) { .range_min = (lo), .range_max = (hi) }
struct regmap_access_table {
	const struct regmap_range *yes_ranges;
	unsigned int n_yes_ranges;
};
struct regmap_config {
	int reg_bits;
	int val_bits;
	int (*reg_read)(void *context, unsigned int reg, unsigned int *val);
	int (*reg_write)(void *context, unsigned int reg, unsigned int val);
	unsigned int max_register;
	const struct regmap_access_table *volatile_table;
	enum regcache_type cache_type;
};
/*
 * The cache the driver's REGCACHE_MAPLE gives it, for what matters here: it
 * starts empty (no defaults, so the first read of a register goes to the
 * part), it holds every value written or read, and a volatile register is
 * never held. A read of a cached register does not reach the part - which is
 * exactly why the driver cannot prove a write today, and why phase 3 of the
 * plan adds a read that bypasses it.
 */
struct regmap {
	void *context;
	const struct regmap_config *config;
	unsigned int cache[256];
	bool cached[256];
	bool bypass;
};
struct regmap *devm_regmap_init(struct device *d, const void *bus, void *ctx,
				const struct regmap_config *config);
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int val);
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask, unsigned int val);
void regcache_cache_bypass(struct regmap *map, bool enable);
int regcache_drop_region(struct regmap *map, unsigned int min, unsigned int max);

/* ----------------------------------------------------------------- IIO */

#define IIO_DMA_MINALIGN	8
enum iio_chan_type { IIO_VOLTAGE, IIO_TIMESTAMP };
enum iio_endian { IIO_CPU, IIO_BE, IIO_LE };
enum iio_chan_info_enum {
	IIO_CHAN_INFO_RAW,
	IIO_CHAN_INFO_SCALE,
	IIO_CHAN_INFO_SAMP_FREQ,
	IIO_CHAN_INFO_HARDWAREGAIN,
};
#define IIO_VAL_INT		1
#define IIO_VAL_INT_PLUS_NANO	3
#define IIO_AVAIL_LIST		0
#define INDIO_DIRECT_MODE	0x01
#define INDIO_BUFFER_SOFTWARE	0x04

struct iio_scan_type {
	char sign;
	u8 realbits;
	u8 storagebits;
	u8 shift;
	enum iio_endian endianness;
};
struct iio_dev;
struct iio_chan_spec;
enum iio_shared_by { IIO_SEPARATE, IIO_SHARED_BY_TYPE, IIO_SHARED_BY_DIR, IIO_SHARED_BY_ALL };
struct iio_chan_spec_ext_info {
	const char *name;
	enum iio_shared_by shared;
	ssize_t (*read)(struct iio_dev *, uintptr_t private,
			const struct iio_chan_spec *, char *buf);
	ssize_t (*write)(struct iio_dev *, uintptr_t private,
			 const struct iio_chan_spec *, const char *buf, size_t len);
	uintptr_t private;
};
struct iio_chan_spec {
	enum iio_chan_type type;
	const struct iio_chan_spec_ext_info *ext_info;
	int channel;
	unsigned long address;
	int scan_index;
	struct iio_scan_type scan_type;
	const char *extend_name;
	long info_mask_separate;
	long info_mask_shared_by_all;
	long info_mask_shared_by_all_available;
	unsigned indexed:1;
};
#define IIO_CHAN_SOFT_TIMESTAMP(_si) {					\
	.type = IIO_TIMESTAMP,						\
	.channel = -1,							\
	.scan_index = _si,						\
	.scan_type = { .sign = 's', .realbits = 64, .storagebits = 64 },	\
}

struct attribute { const char *name; umode_t mode; };
struct device_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
};
struct attribute_group { struct attribute **attrs; };
struct iio_dev_attr { struct device_attribute dev_attr; u64 address; };
#define IIO_DEVICE_ATTR_RW(_name, _addr)					\
	struct iio_dev_attr iio_dev_attr_##_name = {			\
		.dev_attr = { .attr = { .name = #_name, .mode = 0644 },	\
			      .show = _name##_show, .store = _name##_store },	\
		.address = (_addr) }
#define IIO_DEVICE_ATTR(_name, _mode, _show, _store, _addr)		\
	struct iio_dev_attr iio_dev_attr_##_name = {			\
		.dev_attr = { .attr = { .name = #_name, .mode = _mode },	\
			      .show = _show, .store = _store },		\
		.address = (_addr) }
#define to_iio_dev_attr(_dev_attr)	container_of(_dev_attr, struct iio_dev_attr, dev_attr)
#define IIO_DEVICE_ATTR_RO(_name, _addr)					\
	struct iio_dev_attr iio_dev_attr_##_name = {			\
		.dev_attr = { .attr = { .name = #_name, .mode = 0444 },	\
			      .show = _name##_show },				\
		.address = (_addr) }

int sysfs_emit(char *buf, const char *fmt, ...);
int sysfs_emit_at(char *buf, int at, const char *fmt, ...);
int __sysfs_match_string(const char * const *array, size_t n, const char *str);
#define sysfs_match_string(a, s) __sysfs_match_string(a, ARRAY_SIZE(a), s)
bool sysfs_streq(const char *s1, const char *s2);
/* The kernel's: base 10 here, an optional '+', one trailing newline, nothing
 * else; -EINVAL for anything that is not a number, -ERANGE past UINT_MAX. */
int kstrtouint(const char *s, unsigned int base, unsigned int *res);
/* The kernel's: the first character, y/t/1 or n/f/0, or on/off. */
int kstrtobool(const char *s, bool *res);
#ifndef UINT_MAX
#define UINT_MAX	(~0U)
#endif

struct iio_dev;
struct iio_info {
	const struct attribute_group *attrs;
	int (*read_raw)(struct iio_dev *, struct iio_chan_spec const *, int *, int *, long);
	int (*read_avail)(struct iio_dev *, struct iio_chan_spec const *, const int **,
			  int *, int *, long);
	int (*write_raw)(struct iio_dev *, struct iio_chan_spec const *, int, int, long);
	int (*update_scan_mode)(struct iio_dev *, const unsigned long *);
	int (*debugfs_reg_access)(struct iio_dev *, unsigned int, unsigned int, unsigned int *);
};
struct iio_buffer_setup_ops {
	int (*postenable)(struct iio_dev *);
	int (*predisable)(struct iio_dev *);
};
struct iio_dev {
	int modes;
	const char *name;
	const struct iio_info *info;
	const struct iio_chan_spec *channels;
	int num_channels;
	unsigned long *active_scan_mask;
	unsigned int masklength;
	const struct iio_buffer_setup_ops *setup_ops;
	bool buffer_enabled;
	bool registered;
	struct device dev;
	unsigned long scan_mask_storage[1];
	u8 priv[] __aligned(32);
};
static inline void *iio_priv(struct iio_dev *d) { return d->priv; }
#define dev_to_iio_dev(d)	container_of(d, struct iio_dev, dev)
struct iio_dev *devm_iio_device_alloc(struct device *parent, size_t priv_size);
int devm_iio_device_register(struct device *d, struct iio_dev *indio);
int devm_iio_kfifo_buffer_setup(struct device *d, struct iio_dev *indio,
				const struct iio_buffer_setup_ops *ops);
static inline bool iio_buffer_enabled(struct iio_dev *d) { return d->buffer_enabled; }
static inline int iio_device_claim_direct_mode(struct iio_dev *d)
{
	return d->buffer_enabled ? -EBUSY : 0;
}
static inline void iio_device_release_direct_mode(struct iio_dev *d) { (void)d; }
s64 iio_get_time_ns(const struct iio_dev *d);
int iio_push_to_buffers_with_timestamp(struct iio_dev *d, void *data, s64 ts);

#endif /* ADS1299_TEST_KERNEL_SHIM_H */
