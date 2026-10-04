/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The slice of drivers/counter that hid-mcp2210.c uses. Enough to register one
 * Count with one Signal and one Synapse, and to call the ops from a test.
 */
#include "kernel_shim_dev.h"

#ifndef MCP2210_TEST_COUNTER_H
#define MCP2210_TEST_COUNTER_H

#define CONFIG_COUNTER 1

enum counter_function { COUNTER_FUNCTION_INCREASE };
/* Same order as include/uapi/linux/counter.h, so a value means the same thing. */
enum counter_synapse_action {
	COUNTER_SYNAPSE_ACTION_NONE,
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
	COUNTER_SYNAPSE_ACTION_FALLING_EDGE,
	COUNTER_SYNAPSE_ACTION_BOTH_EDGES,
};
enum counter_signal_level { COUNTER_SIGNAL_LEVEL_LOW, COUNTER_SIGNAL_LEVEL_HIGH };

struct counter_signal { int id; const char *name; };

struct counter_synapse {
	const enum counter_synapse_action *actions_list;
	size_t num_actions;
	struct counter_signal *signal;
};

struct counter_count {
	int id;
	const char *name;
	const enum counter_function *functions_list;
	size_t num_functions;
	struct counter_synapse *synapses;
	size_t num_synapses;
};

struct counter_device;

struct counter_ops {
	int (*signal_read)(struct counter_device *, struct counter_signal *,
			   enum counter_signal_level *);
	int (*count_read)(struct counter_device *, struct counter_count *, u64 *);
	int (*count_write)(struct counter_device *, struct counter_count *, u64);
	int (*function_read)(struct counter_device *, struct counter_count *,
			     enum counter_function *);
	int (*action_read)(struct counter_device *, struct counter_count *,
			   struct counter_synapse *, enum counter_synapse_action *);
};

struct counter_device {
	const char *name;
	struct device *parent;
	const struct counter_ops *ops;
	struct counter_signal *signals;
	size_t num_signals;
	struct counter_count *counts;
	size_t num_counts;
	void *shim_priv;
};

/*
 * The one the test reaches for: the driver registers exactly one counter per
 * device, so the shim keeps the last one and the test calls its ops directly.
 */
extern struct counter_device *shim_counter;

static inline void *counter_priv(const struct counter_device *counter)
{
	return counter->shim_priv;
}

static inline struct counter_device *
devm_counter_alloc(struct device *dev, size_t sizeof_priv)
{
	struct counter_device *c = kzalloc(sizeof(*c), GFP_KERNEL);

	(void)dev;
	if (!c)
		return NULL;
	c->shim_priv = kzalloc(sizeof_priv, GFP_KERNEL);
	if (!c->shim_priv) {
		kfree(c);
		return NULL;
	}
	return c;
}

static inline int devm_counter_add(struct device *dev,
				   struct counter_device *counter)
{
	(void)dev;
	shim_counter = counter;
	return 0;
}

static inline const char *dev_name(struct device *dev)
{
	(void)dev;
	return "mcp2210-test";
}

#endif /* MCP2210_TEST_COUNTER_H */
