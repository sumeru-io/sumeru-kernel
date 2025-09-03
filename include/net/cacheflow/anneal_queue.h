/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_ANNEAL_QUEUE_H
#define __CACHEFLOW_ANNEAL_QUEUE_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/min_heap.h>
#include <net/cacheflow/cacheflow.h>

/* Forward declaration */
struct netmem_mini_array;

/* Default anneal queue depth per core - can be tuned via module parameter */
#define ANNEAL_QUEUE_DEPTH_PER_CORE 64

/**
 * struct anneal_core_queue - Per-core cooling queue using fixed-size array
 * @mini_arrays: Fixed-size array of mini array pointers for this core
 * @head: Head index for circular queue (oldest element)
 * @tail: Tail index for circular queue (newest element)
 * @count: Current number of mini arrays in this core's queue
 * @core_id: Core ID this queue belongs to
 * @max_depth: Maximum depth of this core's queue
 * @heap_idx: Backpointer to position in heap (INVALID_HEAP_IDX if not in heap)
 */
struct anneal_core_queue {
	struct netmem_mini_array **mini_arrays;
	u32 head;
	u32 tail;  
	u32 count;
	u16 core_id;
	u16 max_depth;
	u16 heap_idx;  /* Backpointer for O(1) heap updates */
} ____cacheline_aligned_in_smp;

#define INVALID_HEAP_IDX 0xFFFF

/**
 * struct anneal_heap_node - Max heap node for core list prioritization
 * @count: Number of elements in the core list (heap key)
 * @core_id: Core ID this node represents
 */
struct anneal_heap_node {
	u32 count;
	u16 core_id;
};

/* Define heap type with preallocated storage for anneal heap nodes */
MIN_HEAP_PREALLOCATED(struct anneal_heap_node, anneal_heap, CACHEFLOW_MAX_CORES);

/**
 * struct anneal_queue - Per-core buffer cooling system using 2D array
 * @core_queues: Array of per-core cooling queues
 * @storage: 2D array storage for all mini array pointers [CORES][DEPTH]
 * @heap: Max heap for efficient longest-queue lookup using kernel's min_heap
 * @max_cores: Maximum number of cores supported
 * @queue_depth: Maximum depth per core queue
 * @lock: Spinlock protecting the entire anneal queue
 * @total_count: Total number of mini arrays across all core queues
 *
 * Note: Overflow handling is done via pool->stack, not within anneal_queue
 */
struct anneal_queue {
	struct anneal_core_queue core_queues[CACHEFLOW_MAX_CORES];
	struct netmem_mini_array *storage[CACHEFLOW_MAX_CORES][ANNEAL_QUEUE_DEPTH_PER_CORE];
	struct anneal_heap heap;
	u16 queue_depth;
	spinlock_t lock;
	u32 total_count;
} ____cacheline_aligned_in_smp;

/* Anneal queue functions for per-core cooling */

/**
 * anneal_queue_init - Initialize the anneal queue system
 * @aq: Anneal queue to initialize
 * @anneal_size: Number of pages to cache per core (converted to mini arrays per core)
 *
 * Initializes the per-core cooling lists and max heap. The anneal_size is
 * converted from pages to mini arrays for each core's queue depth.
 */
void anneal_queue_init(struct anneal_queue *aq, u32 anneal_size);

/**
 * anneal_queue_enqueue - Add a mini array to the appropriate core's cooling list
 * @aq: Anneal queue
 * @mini_array: Mini array to enqueue for cooling
 *
 * Places the mini array in the cooling list for its assigned core and updates
 * the max heap to maintain priority order. If core queue is full, attempts to
 * use global stack for overflow handling.
 * 
 * Return: 0 on success, -ENOSPC if both core queue and global stack are full
 */
int anneal_queue_enqueue(struct anneal_queue *aq, struct netmem_mini_array *mini_array);

/**
 * anneal_queue_dequeue - Get a mini array from the longest cooling list
 * @aq: Anneal queue
 *
 * Returns a mini array from the core with the most cooled buffers, or from
 * global stack if no core queues are active. Implements load balancing by
 * moving items from global stack to core queues when utilization is low.
 *
 * Return: Mini array from longest cooling list or global stack, or NULL if empty
 */
struct netmem_mini_array *anneal_queue_dequeue(struct anneal_queue *aq);

/**
 * anneal_queue_count - Get total number of cooling mini arrays
 * @aq: Anneal queue
 *
 * Return: Total number of mini arrays across all cooling lists and global stack
 */
static inline u32 anneal_queue_count(const struct anneal_queue *aq)
{
	return aq ? aq->total_count : 0;
}

/**
 * anneal_queue_is_empty - Check if all cooling lists and global stack are empty
 * @aq: Anneal queue
 *
 * Return: true if no mini arrays are cooling, false otherwise
 */
static inline bool anneal_queue_is_empty(const struct anneal_queue *aq)
{
	return aq ? (aq->total_count == 0) : true;
}

#endif /* __CACHEFLOW_ANNEAL_QUEUE_H */