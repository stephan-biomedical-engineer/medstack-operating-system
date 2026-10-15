// SPDX-License-Identifier: GPL-2.0-only
/* The shim's out-of-line half - see kernel_shim.h. */

#include "kernel_shim.h"
#include "fake_ads1299.h"
#include "shim_test.h"

int shim_verbose;
char shim_last_error[256];
unsigned long shim_sleeps;
struct regulator *shim_vref;
struct regulator shim_avdd = { .uv = 5000000 };
irq_handler_t shim_irq_handler;
void *shim_irq_dev;
struct iio_dev *shim_iio;
struct shim_scan shim_scans[SHIM_MAX_SCANS];
unsigned int shim_scan_count;
static s64 shim_clock_ns = 1000000000;

static struct spi_message *pending[8];
static unsigned int pending_count;

void shim_log(bool error, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (error)
		snprintf(shim_last_error, sizeof(shim_last_error), "%s", line);
	if (shim_verbose)
		printf("          [kernel] %s", line);
}

void shim_reset(void)
{
	shim_last_error[0] = '\0';
	shim_sleeps = 0;
	shim_vref = NULL;
	shim_irq_handler = NULL;
	shim_irq_dev = NULL;
	shim_iio = NULL;
	shim_scan_count = 0;
	pending_count = 0;
}

char *devm_kasprintf(struct device *d, gfp_t g, const char *fmt, ...)
{
	char *out = NULL;
	va_list ap;

	(void)d; (void)g;
	va_start(ap, fmt);
	if (vasprintf(&out, fmt, ap) < 0)
		out = NULL;
	va_end(ap);
	return out;
}

int devm_add_action_or_reset(struct device *d, void (*fn)(void *), void *data)
{
	(void)d; (void)fn; (void)data;
	return 0;
}

/* ----------------------------------------------------------------- SPI */

int spi_sync_transfer(struct spi_device *spi, struct spi_transfer *x, unsigned int n)
{
	(void)spi;
	for (unsigned int i = 0; i < n; i++) {
		int ret = fake_transfer(x[i].tx_buf, x[i].rx_buf, x[i].len);

		if (ret)
			return ret;
	}
	return 0;
}

int spi_sync(struct spi_device *spi, struct spi_message *m)
{
	return spi_sync_transfer(spi, m->xfers, m->n);
}

int spi_async(struct spi_device *spi, struct spi_message *m)
{
	(void)spi;
	if (pending_count >= ARRAY_SIZE(pending))
		return -EBUSY;
	pending[pending_count++] = m;
	return 0;
}

unsigned int shim_spi_pending(void)
{
	return pending_count;
}

bool shim_spi_complete_one(void)
{
	struct spi_message *m;

	if (!pending_count)
		return false;
	m = pending[0];
	memmove(pending, pending + 1, (pending_count - 1) * sizeof(pending[0]));
	pending_count--;
	(void)spi_sync_transfer(NULL, m->xfers, m->n);
	if (m->complete)
		m->complete(m->context);
	return true;
}

/* -------------------------------------------------------------- regmap */

static bool is_volatile(const struct regmap *map, unsigned int reg)
{
	const struct regmap_access_table *t = map->config->volatile_table;

	if (!t)
		return false;
	for (unsigned int i = 0; i < t->n_yes_ranges; i++)
		if (reg >= t->yes_ranges[i].range_min && reg <= t->yes_ranges[i].range_max)
			return true;
	return false;
}

struct regmap *devm_regmap_init(struct device *d, const void *bus, void *ctx,
				const struct regmap_config *config)
{
	struct regmap *map = calloc(1, sizeof(*map));

	(void)d; (void)bus;
	map->context = ctx;
	map->config = config;
	return map;
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val)
{
	int ret;

	if (reg > map->config->max_register)
		return -EINVAL;
	if (!map->bypass && !is_volatile(map, reg) && map->cached[reg]) {
		*val = map->cache[reg];
		return 0;
	}
	ret = map->config->reg_read(map->context, reg, val);
	if (ret)
		return ret;
	if (!map->bypass && !is_volatile(map, reg)) {
		map->cache[reg] = *val;
		map->cached[reg] = true;
	}
	return 0;
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int val)
{
	int ret;

	if (reg > map->config->max_register)
		return -EINVAL;
	ret = map->config->reg_write(map->context, reg, val);
	if (ret)
		return ret;
	if (!map->bypass && !is_volatile(map, reg)) {
		map->cache[reg] = val;
		map->cached[reg] = true;
	}
	return 0;
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask, unsigned int val)
{
	unsigned int orig;
	unsigned int tmp;
	int ret;

	ret = regmap_read(map, reg, &orig);
	if (ret)
		return ret;
	tmp = (orig & ~mask) | (val & mask);
	/* Like the kernel's: a write only when the value changes. */
	if (tmp == orig)
		return 0;
	return regmap_write(map, reg, tmp);
}

void regcache_cache_bypass(struct regmap *map, bool enable)
{
	map->bypass = enable;
}

/* --------------------------------------------------------------- sysfs */

int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, 4096, fmt, ap);
	va_end(ap);
	return n;
}

int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf + at, (size_t)(4096 - at), fmt, ap);
	va_end(ap);
	return n;
}

int __sysfs_match_string(const char * const *array, size_t n, const char *str)
{
	for (size_t i = 0; i < n; i++) {
		size_t len;

		if (!array[i])
			continue;
		len = strlen(array[i]);
		/* sysfs_streq: a trailing newline is not part of the value */
		if (!strncmp(array[i], str, len) &&
		    (str[len] == '\0' || (str[len] == '\n' && str[len + 1] == '\0')))
			return (int)i;
	}
	return -EINVAL;
}

/* ----------------------------------------------------------------- IIO */

struct iio_dev *devm_iio_device_alloc(struct device *parent, size_t priv_size)
{
	struct iio_dev *d = calloc(1, sizeof(*d) + priv_size + 64);

	(void)parent;
	d->dev.name = "iio:device0";
	shim_iio = d;
	return d;
}

int devm_iio_device_register(struct device *d, struct iio_dev *indio)
{
	(void)d;
	indio->registered = true;
	return 0;
}

int devm_iio_kfifo_buffer_setup(struct device *d, struct iio_dev *indio,
				const struct iio_buffer_setup_ops *ops)
{
	(void)d;
	indio->setup_ops = ops;
	return 0;
}

s64 iio_get_time_ns(const struct iio_dev *d)
{
	(void)d;
	shim_clock_ns += 1000;
	return shim_clock_ns;
}

int iio_push_to_buffers_with_timestamp(struct iio_dev *d, void *data, s64 ts)
{
	struct shim_scan *s;
	unsigned int n = 0;

	if (shim_scan_count >= SHIM_MAX_SCANS)
		return -ENOSPC;
	s = &shim_scans[shim_scan_count++];
	memset(s, 0, sizeof(*s));
	for (int i = 0; i < d->num_channels && n < 9; i++)
		if (d->channels[i].type == IIO_VOLTAGE &&
		    test_bit((unsigned int)d->channels[i].scan_index, d->active_scan_mask))
			n++;
	memcpy(s->value, data, n * sizeof(u32));
	s->count = n;
	s->timestamp = ts;
	return 0;
}
