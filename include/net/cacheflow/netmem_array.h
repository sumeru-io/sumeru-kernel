/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_NETMEM_ARRAY_H
#define __CACHEFLOW_NETMEM_ARRAY_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/compiler.h>
#include <net/page_pool/helpers.h>

/* Size array to fit within two cachelines minus the metadata fields */
#define CF_PP_MINI_ARRAY_METADATA_SIZE				8
#define CF_PP_MINI_ARRAY_SIZE 					(((2 * L1_CACHE_BYTES) - CF_PP_MINI_ARRAY_METADATA_SIZE) / sizeof(netmem_ref))
#define CF_PP_EMPTY_MINI_ARRAY_GLBOAL_CACHE_SIZE 		1024

/**
 * struct netmem_mini_array - Network memory mini array container
 * @array:	Array of network memory references
 * @count:	Number of valid entries in array (0 <= count <= CF_PP_MINI_ARRAY_SIZE)
 * @core:	CPU core assignment hint for affinity
 * @flags:	Control flags for array behavior
 *
 * This structure holds a batch of network memory references with metadata
 * for efficient bulk operations. The count field maintains the invariant
 * that it equals the number of valid entries in the array.
 */
struct netmem_mini_array {
	netmem_ref array[CF_PP_MINI_ARRAY_SIZE];
	u16 count;
	u16 core;
	int flags;
} ____cacheline_aligned_in_smp;

/**
 * struct netmem_empty_mini_array_global_cache - Global cache for empty mini arrays
 * @array:	Array of pointers to empty mini arrays
 * @count:	Number of cached empty arrays available
 * @lock:	Spinlock protecting concurrent access to the cache
 *
 * This structure provides a global cache of pre-allocated empty mini arrays
 * to reduce allocation overhead across all page pools.
 */
struct netmem_empty_mini_array_global_cache {
	struct netmem_mini_array *array[CF_PP_EMPTY_MINI_ARRAY_GLBOAL_CACHE_SIZE];
	u32 count;
	spinlock_t lock;
};

/* Helper functions for safe netmem_mini_array operations */

/**
 * netmem_mini_array_init - Initialize a mini array to empty state
 * @array: Mini array to initialize
 *
 * Sets count to 0 and clears core and flags fields.
 */
static inline void netmem_mini_array_init(struct netmem_mini_array *array)
{
	array->count = 0;
	array->core = 0;
	array->flags = 0;
}

/**
 * netmem_mini_array_is_full - Check if mini array is at maximum capacity
 * @array: Mini array to check
 *
 * Return: true if array is full, false otherwise
 */
static inline bool netmem_mini_array_is_full(const struct netmem_mini_array *array)
{
	return array->count >= CF_PP_MINI_ARRAY_SIZE;
}

/**
 * netmem_mini_array_is_empty - Check if mini array is empty
 * @array: Mini array to check
 *
 * Return: true if array is empty, false otherwise
 */
static inline bool netmem_mini_array_is_empty(const struct netmem_mini_array *array)
{
	return array->count == 0;
}

/**
 * netmem_mini_array_remaining_space - Get number of free slots in array
 * @array: Mini array to check
 *
 * Return: number of free slots available
 */
static inline u16 netmem_mini_array_remaining_space(const struct netmem_mini_array *array)
{
	return CF_PP_MINI_ARRAY_SIZE - array->count;
}

/**
 * netmem_mini_array_push - Add a netmem reference to the array
 * @array: Mini array to push to
 * @netmem: Network memory reference to add
 *
 * Safely adds a netmem reference to the end of the array.
 * 
 * Return: 0 on success, -ENOSPC if array is full
 */
static inline int netmem_mini_array_push(struct netmem_mini_array *array, netmem_ref netmem)
{
	if (unlikely(netmem_mini_array_is_full(array)))
		return -ENOSPC;
	
	array->array[array->count] = netmem;
	array->count++;
	return 0;
}

/**
 * netmem_mini_array_pop - Remove and return the last netmem reference
 * @array: Mini array to pop from
 *
 * Safely removes the last netmem reference from the array.
 * 
 * Return: netmem reference, or 0 if array is empty
 */
static inline netmem_ref netmem_mini_array_pop(struct netmem_mini_array *array)
{
	netmem_ref netmem;
	
	if (unlikely(netmem_mini_array_is_empty(array)))
		return 0;
	
	array->count--;
	netmem = array->array[array->count];
	array->array[array->count] = 0; /* Clear for safety */
	
	return netmem;
}

/**
 * netmem_mini_array_peek - Get the last netmem reference without removing it
 * @array: Mini array to peek at
 *
 * Return: netmem reference, or 0 if array is empty
 */
static inline netmem_ref netmem_mini_array_peek(const struct netmem_mini_array *array)
{
	if (unlikely(netmem_mini_array_is_empty(array)))
		return 0;
	
	return array->array[array->count - 1];
}


#endif /* __CACHEFLOW_NETMEM_ARRAY_H */