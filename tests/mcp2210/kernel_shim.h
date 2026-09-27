/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The subset of the kernel that hid-mcp2210.c touches, reimplemented for the
 * host.
 *
 * Why this exists rather than a KUnit test: the seam this suite needs is the
 * USB report exchange, and in-tree that seam is hid_hw_output_report() plus a
 * completion filled from an interrupt endpoint. Substituting it inside the
 * kernel would mean putting a function pointer or an #ifdef into the driver,
 * which is test scaffolding in a file meant to be posted to linux-input. Here
 * the substitution happens one level below the driver, in an API the driver
 * only calls - so hid-mcp2210.c is compiled BYTE FOR BYTE as it ships, with no
 * MCP2210_TEST macro and no seam of its own.
 *
 * What is faithful and what is not:
 *
 *  - Faithful: the report exchange is synchronous and one deep, which is what
 *    the driver's mutex already guarantees; a reply not produced is a
 *    wait_for_completion_timeout() that expires; a reply produced late is
 *    delivered on a later exchange, exactly as a slow device would.
 *  - Not faithful: there is no concurrency, no real sleeping, no memory
 *    barriers and no allocation failure. Anything this suite says about
 *    locking or about races is worth nothing.
 */

#ifndef MCP2210_TEST_KERNEL_SHIM_H
#define MCP2210_TEST_KERNEL_SHIM_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

#define __packed __attribute__((packed))
#define __init
#define __exit
#define __maybe_unused __attribute__((unused))

#define BIT(n)			(1UL << (n))
#define GENMASK(h, l)		(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (64 - 1 - (h))))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))

#define min(a, b)		((a) < (b) ? (a) : (b))
#define max(a, b)		((a) > (b) ? (a) : (b))
#define min_t(t, a, b)		((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define swap(a, b)		do { typeof(a) __t = (a); (a) = (b); (b) = __t; } while (0)

#define __ffs(x)		__builtin_ctzl(x)
#define FIELD_PREP(mask, val)	(((val) << __ffs(mask)) & (mask))
#define FIELD_GET(mask, val)	(((val) & (mask)) >> __ffs(mask))

#define IS_REACHABLE(opt)	1
#define CONFIG_GPIOLIB		1
#define THIS_MODULE		NULL

/* ------------------------------------------------------------ allocation */

#define GFP_KERNEL		0
static inline void *kmemdup(const void *src, size_t len, int gfp)
{
	void *p = malloc(len);

	(void)gfp;
	if (p)
		memcpy(p, src, len);
	return p;
}
static inline void *kzalloc(size_t len, int gfp) { (void)gfp; return calloc(1, len); }
static inline void kfree(const void *p) { free((void *)p); }

/* ------------------------------------------------------------ unaligned */

static inline u16 get_unaligned_le16(const void *p)
{
	const u8 *b = p;

	return (u16)(b[0] | ((u16)b[1] << 8));
}
static inline u32 get_unaligned_le32(const void *p)
{
	const u8 *b = p;

	return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}
static inline void put_unaligned_le16(u16 v, void *p)
{
	u8 *b = p;

	b[0] = (u8)v;
	b[1] = (u8)(v >> 8);
}
static inline void put_unaligned_le32(u32 v, void *p)
{
	u8 *b = p;

	b[0] = (u8)v;
	b[1] = (u8)(v >> 8);
	b[2] = (u8)(v >> 16);
	b[3] = (u8)(v >> 24);
}

/* ------------------------------------------------------- mutex, sleeping */

struct mutex { int held; };
static inline void mutex_init(struct mutex *m) { m->held = 0; }
static inline void mutex_lock(struct mutex *m) { m->held++; }
static inline void mutex_unlock(struct mutex *m) { m->held--; }

/*
 * Real elapsed time is not modelled: the suite counts iterations, which is the
 * quantity the driver's own stall bound is written in. A test that needed wall
 * clock would be measuring this shim.
 */
extern unsigned long shim_usleep_calls;
static inline void usleep_range(unsigned long a, unsigned long b)
{
	(void)a;
	(void)b;
	shim_usleep_calls++;
}
static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }

/* ---------------------------------------------------------- completion */

struct completion { int done; };
static inline void init_completion(struct completion *c) { c->done = 0; }
static inline void reinit_completion(struct completion *c) { c->done = 0; }
static inline void complete(struct completion *c) { c->done = 1; }
static inline unsigned long
wait_for_completion_timeout(struct completion *c, unsigned long t)
{
	(void)t;
	/*
	 * The fake device answers inside hid_hw_output_report(), so by the time
	 * the driver waits the answer is either already here or was never going
	 * to come. Returning 0 is the timeout the driver turns into -ETIMEDOUT.
	 */
	return c->done ? 1 : 0;
}

/* --------------------------------------------------------------- string */

static inline void strscpy(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	if (!n)
		return;
	for (; i < n - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

#endif /* MCP2210_TEST_KERNEL_SHIM_H */
