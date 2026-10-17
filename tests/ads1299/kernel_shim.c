// SPDX-License-Identifier: GPL-2.0-only
/* The shim's out-of-line half - see kernel_shim.h. */

#include "kernel_shim.h"
#include "fake_ads1299.h"
#include "shim_test.h"

int shim_verbose;
char shim_last_error[256];
char shim_error_log[2048];
unsigned long shim_sleeps;
struct regulator *shim_vref;
struct regulator *shim_biasref;
struct regulator shim_avdd = { .uv = 5000000 };
irq_handler_t shim_irq_handler;
void *shim_irq_dev;
struct iio_dev *shim_iio;
struct shim_scan shim_scans[SHIM_MAX_SCANS];
unsigned int shim_scan_count;
unsigned int shim_cache_drops;
unsigned int shim_spi_setups;
bool shim_spi_setup_rt;
static s64 shim_clock_ns = 1000000000;
bool shim_prop_clock_cells;
u32 shim_prop_daisy;
const char *shim_prop_clock_name;
struct clk *shim_ext_clk;
struct clk_hw *shim_clk_hw;
char shim_clk_hw_name[64];
const struct clk_ops *shim_clk_hw_ops;

static struct spi_message *pending[8];
static unsigned int pending_count;

void shim_log(bool error, const char *fmt, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (error) {
		size_t used = strlen(shim_error_log);

		snprintf(shim_last_error, sizeof(shim_last_error), "%s", line);
		snprintf(shim_error_log + used, sizeof(shim_error_log) - used, "%s", line);
	}
	if (shim_verbose)
		printf("          [kernel] %s", line);
}

void shim_reset(void)
{
	shim_last_error[0] = '\0';
	shim_error_log[0] = '\0';
	shim_sleeps = 0;
	shim_vref = NULL;
	shim_biasref = NULL;
	shim_irq_handler = NULL;
	shim_irq_dev = NULL;
	shim_iio = NULL;
	shim_scan_count = 0;
	shim_cache_drops = 0;
	shim_spi_setups = 0;
	shim_spi_setup_rt = false;
	pending_count = 0;
	shim_prop_clock_cells = false;
	shim_prop_daisy = 0;
	shim_prop_clock_name = NULL;
	shim_ext_clk = NULL;
	shim_clk_hw = NULL;
	shim_clk_hw_name[0] = '\0';
	shim_clk_hw_ops = NULL;
}

bool device_property_present(const struct device *d, const char *name)
{
	(void)d;
	if (!strcmp(name, "#clock-cells"))
		return shim_prop_clock_cells;
	if (!strcmp(name, "#daisy-chained-devices"))
		return shim_prop_daisy != 0;
	if (!strcmp(name, "clock-output-names"))
		return shim_prop_clock_name != NULL;
	return false;
}

int device_property_read_u32(const struct device *d, const char *name, u32 *val)
{
	(void)d;
	if (!strcmp(name, "#daisy-chained-devices") && shim_prop_daisy) {
		*val = shim_prop_daisy;
		return 0;
	}
	return -EINVAL;
}

int device_property_read_string(const struct device *d, const char *name,
				const char **val)
{
	(void)d;
	if (!strcmp(name, "clock-output-names") && shim_prop_clock_name) {
		*val = shim_prop_clock_name;
		return 0;
	}
	return -EINVAL;
}

/*
 * The init data lives on the registering function's stack, as in the kernel,
 * so what the test needs from it is copied here.
 */
int devm_clk_hw_register(struct device *d, struct clk_hw *hw)
{
	(void)d;
	shim_clk_hw = hw;
	snprintf(shim_clk_hw_name, sizeof(shim_clk_hw_name), "%s", hw->init->name);
	shim_clk_hw_ops = hw->init->ops;
	return 0;
}

struct clk_hw *of_clk_hw_simple_get(void *spec, void *data)
{
	(void)spec;
	return data;
}

int devm_of_clk_add_hw_provider(struct device *d,
				struct clk_hw *(*get)(void *, void *), void *data)
{
	(void)d; (void)get; (void)data;
	return 0;
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
	/*
	 * The cache first and the bus after, in that order, as _regmap_write()
	 * does in the kernel this driver ships with (6.6,
	 * drivers/base/regmap/regmap.c): a write the bus refuses leaves its
	 * value in the cache. This shim had it the other way round until phase
	 * 3, which is the order in which that defect cannot be seen.
	 */
	if (!map->bypass && !is_volatile(map, reg)) {
		map->cache[reg] = val;
		map->cached[reg] = true;
	}
	ret = map->config->reg_write(map->context, reg, val);
	return ret;
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

int regcache_drop_region(struct regmap *map, unsigned int min, unsigned int max)
{
	for (unsigned int r = min; r <= max && r <= map->config->max_register; r++)
		map->cached[r] = false;
	shim_cache_drops++;
	return 0;
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

bool sysfs_streq(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	if (*s1 == *s2)
		return true;
	if (!*s1 && *s2 == '\n' && !s2[1])
		return true;
	if (*s1 == '\n' && !s1[1] && !*s2)
		return true;
	return false;
}

int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	unsigned long long v = 0;
	const char *p = s;

	if (base != 10)
		return -EINVAL;
	if (*p == '+')
		p++;
	if (*p < '0' || *p > '9')
		return -EINVAL;
	for (; *p >= '0' && *p <= '9'; p++) {
		v = v * 10 + (unsigned long long)(*p - '0');
		if (v > UINT_MAX)
			return -ERANGE;
	}
	if (*p == '\n')
		p++;
	if (*p)
		return -EINVAL;
	*res = (unsigned int)v;
	return 0;
}

/* lib/kstrtox.c, as it is: only the first one or two characters count. */
int kstrtobool(const char *s, bool *res)
{
	if (!s)
		return -EINVAL;

	switch (s[0]) {
	case 'y': case 'Y': case 't': case 'T': case '1':
		*res = true;
		return 0;
	case 'n': case 'N': case 'f': case 'F': case '0':
		*res = false;
		return 0;
	case 'o': case 'O':
		switch (s[1]) {
		case 'n': case 'N':
			*res = true;
			return 0;
		case 'f': case 'F':
			*res = false;
			return 0;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return -EINVAL;
}

int spi_setup(struct spi_device *spi)
{
	shim_spi_setups++;
	shim_spi_setup_rt = spi->rt;
	return 0;
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
	for (int i = 0; i < d->num_channels && n < SHIM_SCAN_VALUES; i++)
		if (d->channels[i].type == IIO_VOLTAGE &&
		    test_bit((unsigned int)d->channels[i].scan_index, d->active_scan_mask))
			n++;
	memcpy(s->value, data, n * sizeof(u32));
	s->count = n;
	s->timestamp = ts;
	return 0;
}
