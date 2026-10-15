/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What the shim records for the tests, and the handles they drive it by.
 */

#ifndef ADS1299_TEST_SHIM_TEST_H
#define ADS1299_TEST_SHIM_TEST_H

#include "kernel_shim.h"

/* A scan the driver pushed to the IIO buffer, as the buffer received it. */
struct shim_scan {
	u32 value[9];
	unsigned int count;	/* channels in it, the timestamp apart */
	s64 timestamp;
};
#define SHIM_MAX_SCANS	4096

extern struct iio_dev *shim_iio;	/* the device the driver allocated */
extern struct shim_scan shim_scans[SHIM_MAX_SCANS];
extern unsigned int shim_scan_count;
extern unsigned int shim_cache_drops;	/* regcache_drop_region() calls */

void shim_reset(void);
/* Transfers started with spi_async(), held until the test completes them. */
unsigned int shim_spi_pending(void);
bool shim_spi_complete_one(void);

#endif /* ADS1299_TEST_SHIM_TEST_H */
