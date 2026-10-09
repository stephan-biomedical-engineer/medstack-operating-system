/* SPDX-License-Identifier: MIT */
/* The converter on the 40-pin header, driven by the M33 - see afe.c. */

#ifndef AFE_H
#define AFE_H

#include <stdint.h>

#include "afe_regs.h"

enum {
    AFE_OK = 0,
    AFE_NOT_OWNED = 1, /* RIF: a resource belongs to another core */
    AFE_SPI_ERROR = 2,
    AFE_WRONG_ID = 3,
};

/* One conversion, as the DRDY interrupt read it: when, and the 27 raw bytes. */
struct afe_sample {
    uint64_t at_us;
    uint8_t raw[AFE_FRAME_BYTES];
};

/* Supplied by main.c: microseconds on the M33's clock, safe to call from an
 * interrupt of higher priority than SysTick. */
uint64_t med_now_us(void);

/* Ownership first, then power-up, reset and the ID. Touches nothing it does
 * not own. Reports to the trace buffer. */
int afe_probe(void);

/* Stop converting, write the register image, read it back. */
int afe_apply(const struct afe_regs *regs);

/* Continuous conversion: RDATAC, START, and the DRDY interrupt armed. */
int afe_start(void);
void afe_stop(void);

/* Stop and drive PWDN low: the state a stopped device leaves its converter in. */
void afe_power_down(void);

/* Take one conversion the interrupt queued. 1 if there was one. */
int afe_take(struct afe_sample *out);

/* Counters for the trace log. */
struct afe_counters {
    uint32_t drdy;          /* interrupts taken */
    uint32_t overruns;      /* conversions lost because the queue was full */
    uint32_t spi_errors;    /* reads that failed */
    uint32_t spi_us_last;   /* duration of the last 27-byte read */
};
void afe_counters(struct afe_counters *out);

#endif /* AFE_H */
