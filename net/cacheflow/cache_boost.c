// SPDX-License-Identifier: GPL-2.0
/*
 * CacheFlow Cache Boost - Direct MSR-based LLC way allocation
 *
 *
 * Author: Minhu Wang <minhuw@acm.org>
 */

#include <linux/types.h>
#include <linux/export.h>
#include <linux/bitops.h>
#include <asm/msr.h>
#include <net/cacheflow/cacheflow.h>
#include <trace/events/cacheflow.h>

/* Intel CAT MSR definitions */
#define IA32_L3_QOS_MASK_BASE   0xC90    /* Base MSR for L3 CBM */
#define IA32_PQR_ASSOC          0xC8F    /* Thread->COS association */

/* Cache boost configuration parameters are defined in net/cacheflow/cacheflow.c
 * and exposed via sysctl interface in net/core/sysctl_net_core.c */

/**
 * cacheflow_cache_cbm_to_ways - Count number of ways in CBM
 * @cbm: Cache Bit Mask
 *
 * Count set bits to determine number of allocated cache ways.
 *
 * Return: Number of cache ways
 */
static inline u32 cacheflow_cache_cbm_to_ways(u64 cbm)
{
	return hweight64(cbm);
}

/**
 * cacheflow_cache_add_way - Add one cache way to CBM
 * @cbm: Current Cache Bit Mask
 *
 * Add one contiguous cache way to the existing mask.
 * Maintains contiguous bit requirement of Intel CAT.
 * Expansion direction controlled by cacheflow_cache_expand_left:
 * 0 = expand right first (toward lower bits), then left
 * 1 = expand left first (toward higher bits), then right
 *
 * Return: New CBM with one additional way, or original if can't add
 */
static u64 cacheflow_cache_add_way(u64 cbm)
{
	u64 new_cbm;
	int rightmost_bit, leftmost_bit;
	int expand_left = READ_ONCE(cacheflow_cache_expand_left);

	if (!cbm)
		return 0x1; /* Start with first way */

	/* Find rightmost and leftmost set bits */
	rightmost_bit = __ffs64(cbm);
	leftmost_bit = 63 - __builtin_clzll(cbm);

	if (expand_left) {
		/* Expand left first (toward higher bits), then right */
		if (leftmost_bit < 63) {
			new_cbm = cbm | (1ULL << (leftmost_bit + 1));
			return new_cbm;
		}
		if (rightmost_bit > 0) {
			new_cbm = cbm | (1ULL << (rightmost_bit - 1));
			return new_cbm;
		}
	} else {
		/* Expand right first (toward lower bits), then left */
		if (rightmost_bit > 0) {
			new_cbm = cbm | (1ULL << (rightmost_bit - 1));
			return new_cbm;
		}
		if (leftmost_bit < 63) {
			new_cbm = cbm | (1ULL << (leftmost_bit + 1));
			return new_cbm;
		}
	}

	/* Can't add more ways */
	return cbm;
}

/**
 * cacheflow_cache_remove_way - Remove one cache way from CBM
 * @cbm: Current Cache Bit Mask
 *
 * Remove one cache way while maintaining contiguous bits.
 *
 * Return: New CBM with one fewer way, or original if can't remove
 */
static u64 cacheflow_cache_remove_way(u64 cbm)
{
	u64 new_cbm;
	int rightmost_bit, leftmost_bit;

	if (!cbm || cbm == 0x1)
		return cbm; /* Can't remove from empty or single way */

	/* Find rightmost and leftmost set bits */
	rightmost_bit = __ffs64(cbm);
	leftmost_bit = 63 - __builtin_clzll(cbm);

	/* Remove from right (higher numbered way) */
	new_cbm = cbm & ~(1ULL << leftmost_bit);
	if (new_cbm)
		return new_cbm;

	/* Remove from left (lower numbered way) */
	new_cbm = cbm & ~(1ULL << rightmost_bit);
	return new_cbm;
}

/**
 * cacheflow_cache_up - Increase cache allocation by one way
 * @buffer_usage: Current buffer usage in pages (for tracing)
 *
 * Directly increases L3 cache allocation for the configured COS
 * by adding one cache way via MSR programming.
 */
void cacheflow_cache_up(u32 buffer_usage)
{
	u32 cos_id = READ_ONCE(cacheflow_cache_cos);
	u32 max_ways = READ_ONCE(cacheflow_cache_max_ways);
	u64 current_cbm, new_cbm;
	u32 current_ways, new_ways;

	if (!cos_id)  /* Cache boost disabled */
		return;

	/* Read current CBM for this COS */
	rdmsrl(IA32_L3_QOS_MASK_BASE + cos_id, current_cbm);
	current_ways = cacheflow_cache_cbm_to_ways(current_cbm);

	/* Check if we can add more ways */
	if (current_ways >= max_ways)
		return;

	/* Add one cache way */
	new_cbm = cacheflow_cache_add_way(current_cbm);
	new_ways = cacheflow_cache_cbm_to_ways(new_cbm);

	/* Only update if we actually added a way */
	if (new_ways > current_ways) {
		wrmsrl(IA32_L3_QOS_MASK_BASE + cos_id, new_cbm);

		/* Trace the cache allocation change */
		trace_cacheflow_cache_state(buffer_usage, cos_id,
					   current_ways, new_ways);
	}
}
EXPORT_SYMBOL(cacheflow_cache_up);

/**
 * cacheflow_cache_down - Decrease cache allocation by one way
 * @buffer_usage: Current buffer usage in pages (for tracing)
 *
 * Directly decreases L3 cache allocation for the configured COS
 * by removing one cache way via MSR programming.
 */
void cacheflow_cache_down(u32 buffer_usage)
{
	u32 cos_id = READ_ONCE(cacheflow_cache_cos);
	u32 min_ways = READ_ONCE(cacheflow_cache_min_ways);
	u64 current_cbm, new_cbm;
	u32 current_ways, new_ways;

	if (!cos_id)  /* Cache boost disabled */
		return;

	/* Read current CBM for this COS */
	rdmsrl(IA32_L3_QOS_MASK_BASE + cos_id, current_cbm);
	current_ways = cacheflow_cache_cbm_to_ways(current_cbm);

	/* Check if we can remove ways */
	if (current_ways <= min_ways)
		return;

	/* Remove one cache way */
	new_cbm = cacheflow_cache_remove_way(current_cbm);
	new_ways = cacheflow_cache_cbm_to_ways(new_cbm);

	/* Only update if we actually removed a way */
	if (new_ways < current_ways) {
		wrmsrl(IA32_L3_QOS_MASK_BASE + cos_id, new_cbm);

		/* Trace the cache allocation change */
		trace_cacheflow_cache_state(buffer_usage, cos_id,
					   current_ways, new_ways);
	}
}
EXPORT_SYMBOL(cacheflow_cache_down);

/**
 * cacheflow_cache_get_current_ways - Get current cache allocation
 *
 * Return: Number of currently allocated cache ways for CacheFlow COS
 */
u32 cacheflow_cache_get_current_ways(void)
{
	u32 cos_id = READ_ONCE(cacheflow_cache_cos);
	u64 cbm;

	if (!cos_id)
		return 0;

	rdmsrl(IA32_L3_QOS_MASK_BASE + cos_id, cbm);
	return cacheflow_cache_cbm_to_ways(cbm);
}
EXPORT_SYMBOL(cacheflow_cache_get_current_ways);

/**
 * cacheflow_cache_set_ways - Set specific number of cache ways
 * @target_ways: Target number of cache ways
 * @buffer_usage: Current buffer usage (for tracing)
 *
 * Directly set cache allocation to specific number of ways.
 * Used for initialization or direct control.
 *
 * Return: 0 on success, negative on error
 */
int cacheflow_cache_set_ways(u32 target_ways, u32 buffer_usage)
{
	u32 cos_id = READ_ONCE(cacheflow_cache_cos);
	u32 min_ways = READ_ONCE(cacheflow_cache_min_ways);
	u32 max_ways = READ_ONCE(cacheflow_cache_max_ways);
	u64 current_cbm, new_cbm;
	u32 current_ways;

	if (!cos_id)
		return -ENODEV;

	if (target_ways < min_ways || target_ways > max_ways)
		return -EINVAL;

	rdmsrl(IA32_L3_QOS_MASK_BASE + cos_id, current_cbm);
	current_ways = cacheflow_cache_cbm_to_ways(current_cbm);

	/* Create contiguous mask for target_ways */
	new_cbm = (1ULL << target_ways) - 1;

	if (new_cbm != current_cbm) {
		wrmsrl(IA32_L3_QOS_MASK_BASE + cos_id, new_cbm);

		trace_cacheflow_cache_state(buffer_usage, cos_id,
					   current_ways, target_ways);
	}

	return 0;
}
EXPORT_SYMBOL(cacheflow_cache_set_ways);