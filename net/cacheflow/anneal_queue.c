// SPDX-License-Identifier: GPL-2.0
/*
 * Anneal Queue - Per-core buffer cooling system for CacheFlow
 *
 * Author: CacheFlow Team
 * 
 * This implements a per-core cooling mechanism for network memory buffers
 * using a high-performance 2D array approach instead of linked lists.
 * Mini arrays are stored in fixed-size circular queues per core, and 
 * allocation prioritizes cores with the most cooled buffers using the
 * kernel's min_heap.h as a max heap.
 */

#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/min_heap.h>
#include <net/cacheflow/netmem_array.h>
#include <net/cacheflow/anneal_queue.h>

/*
 * Heap callback functions for max heap behavior
 */

static bool anneal_heap_greater_than(const void *lhs, const void *rhs, void *args)
{
	const struct anneal_heap_node *left = lhs;
	const struct anneal_heap_node *right = rhs;
	
	/* For max heap: return true if left > right */
	return left->count > right->count;
}

static void anneal_heap_swap_nodes(void *lhs, void *rhs, void *args)
{
	struct anneal_heap_node *left = lhs;
	struct anneal_heap_node *right = rhs;
	struct anneal_queue *aq = args;
	struct anneal_heap_node temp = *left;
	size_t left_idx = left - aq->heap.data;
	size_t right_idx = right - aq->heap.data;
	u16 left_core_id, right_core_id;
	
	/* Cache core IDs before swap to avoid race conditions */
	left_core_id = left->core_id;
	right_core_id = right->core_id;
	
	/* Swap heap nodes */
	*left = *right;
	*right = temp;
	
	/* Update backpointers atomically with cached core IDs */
	if (likely(left_core_id < CACHEFLOW_MAX_CORES))
		WRITE_ONCE(aq->core_queues[left_core_id].heap_idx, right_idx);
	if (likely(right_core_id < CACHEFLOW_MAX_CORES))
		WRITE_ONCE(aq->core_queues[right_core_id].heap_idx, left_idx);
}

static const struct min_heap_callbacks anneal_heap_callbacks = {
	.less = anneal_heap_greater_than,  /* Use greater_than for max heap */
	.swp = anneal_heap_swap_nodes,
};

/*
 * Core queue operations - all O(1) with 2D array
 */

static inline bool anneal_core_queue_is_full(struct anneal_core_queue *queue)
{
	return queue->count >= queue->max_depth;
}

static inline bool anneal_core_queue_is_empty(struct anneal_core_queue *queue)
{
	return queue->count == 0;
}

static inline int anneal_core_queue_enqueue(struct anneal_core_queue *queue, 
					     struct netmem_mini_array *mini_array)
{
	if (anneal_core_queue_is_full(queue))
		return -ENOSPC;
	
	/* Ensure max_depth is reasonable to prevent overflow */
	if (unlikely(queue->max_depth == 0 || queue->max_depth > ANNEAL_QUEUE_DEPTH_PER_CORE))
		return -EINVAL;
	
	queue->mini_arrays[queue->tail] = mini_array;
	queue->tail = (queue->tail + 1) % queue->max_depth;
	queue->count++;
	
	return 0;
}

static inline struct netmem_mini_array *anneal_core_queue_dequeue(struct anneal_core_queue *queue)
{
	struct netmem_mini_array *mini_array;
	
	if (anneal_core_queue_is_empty(queue))
		return NULL;
	
	/* Ensure max_depth is reasonable to prevent overflow */
	if (unlikely(queue->max_depth == 0 || queue->max_depth > ANNEAL_QUEUE_DEPTH_PER_CORE))
		return NULL;
	
	mini_array = queue->mini_arrays[queue->head];
	queue->mini_arrays[queue->head] = NULL;  /* For debugging */
	queue->head = (queue->head + 1) % queue->max_depth;
	queue->count--;
	
	return mini_array;
}

/*
 * Anneal queue public interface
 */

void anneal_queue_init(struct anneal_queue *aq, u32 anneal_size)
{
	int i;
	u32 per_core_depth;
	
	if (!aq)
		return;
	
	/* Validate anneal_size to prevent integer overflow */
	if (anneal_size > (U32_MAX - CF_PP_MINI_ARRAY_SIZE + 1))
		anneal_size = U32_MAX - CF_PP_MINI_ARRAY_SIZE + 1;
	
	/* Convert pages to mini arrays per core using same formula as simple backend */
	per_core_depth = (anneal_size + CF_PP_MINI_ARRAY_SIZE - 1) / CF_PP_MINI_ARRAY_SIZE;
	
	/* Ensure minimum depth and respect maximum limit */
	per_core_depth = max(per_core_depth, 1U);
	per_core_depth = min(per_core_depth, (u32)ANNEAL_QUEUE_DEPTH_PER_CORE);
	
	aq->queue_depth = per_core_depth;
	aq->total_count = 0;
	spin_lock_init(&aq->lock);
	
	/* Initialize per-core queues with 2D array storage */
	for (i = 0; i < CACHEFLOW_MAX_CORES; i++) {
		struct anneal_core_queue *queue = &aq->core_queues[i];
		
		queue->mini_arrays = aq->storage[i];  /* Point to 2D array row */
		queue->head = 0;
		queue->tail = 0;
		queue->count = 0;
		queue->core_id = i;
		queue->max_depth = aq->queue_depth;
		queue->heap_idx = INVALID_HEAP_IDX;  /* Not in heap initially */
		
		/* Clear the storage array for this core */
		memset(aq->storage[i], 0, sizeof(aq->storage[i]));
	}
	
	/* Initialize heap with preallocated storage */
	min_heap_init(&aq->heap, NULL, CACHEFLOW_MAX_CORES);
}

int anneal_queue_enqueue(struct anneal_queue *aq, struct netmem_mini_array *mini_array)
{
	u16 core_id;
	struct anneal_core_queue *core_queue;
	struct anneal_heap_node new_node;
	unsigned long flags;
	int i, ret;
	
	if (!mini_array || !aq)
		return -EINVAL;
	
	/* Validate and sanitize core_id */
	core_id = mini_array->core;
	if (core_id >= CACHEFLOW_MAX_CORES) {
		/* Use current CPU as fallback for invalid core IDs */
		core_id = smp_processor_id() % CACHEFLOW_MAX_CORES;
		mini_array->core = core_id;  /* Update the mini_array */
	}
	
	spin_lock_irqsave(&aq->lock, flags);
	
	core_queue = &aq->core_queues[core_id];
	
	/* Try to enqueue into core's circular queue */
	ret = anneal_core_queue_enqueue(core_queue, mini_array);
	if (ret != 0) {
		/* Core queue is full - caller will fallback to pool->stack */
		spin_unlock_irqrestore(&aq->lock, flags);
		return -ENOSPC;
	}
	
	aq->total_count++;
	
	/* Update or insert in heap using backpointer pattern */
	if (core_queue->heap_idx != INVALID_HEAP_IDX) {
		/* Core already in heap - O(1) update using backpointer */
		i = core_queue->heap_idx;
		if (likely(i < aq->heap.nr)) {
			aq->heap.data[i].count = core_queue->count;
			/* Only sift up since count increased */
			min_heap_sift_up(&aq->heap, i, &anneal_heap_callbacks, aq);
		}
	} else {
		/* Add new core to heap */
		new_node.core_id = core_id;
		new_node.count = core_queue->count;
		/* Set backpointer only after successful push */
		if (min_heap_push(&aq->heap, &new_node, &anneal_heap_callbacks, aq) == 0)
			core_queue->heap_idx = aq->heap.nr - 1;
	}
	
	spin_unlock_irqrestore(&aq->lock, flags);
	return 0;
}

struct netmem_mini_array *anneal_queue_dequeue(struct anneal_queue *aq)
{
	struct netmem_mini_array *mini_array = NULL;
	struct anneal_core_queue *core_queue;
	struct anneal_heap_node *top_node;
	u16 core_id;
	unsigned long flags;
	
	if (!aq || anneal_queue_is_empty(aq))
		return NULL;
	
	spin_lock_irqsave(&aq->lock, flags);
	
	if (aq->heap.nr == 0) {
		/* No core queues have buffers */
		spin_unlock_irqrestore(&aq->lock, flags);
		return NULL;
	}
	
	/* Get core ID with most cooling buffers (heap root) */
	top_node = min_heap_peek(&aq->heap);
	if (unlikely(!top_node))
		goto unlock_exit;
		
	core_id = top_node->core_id;
	if (unlikely(core_id >= CACHEFLOW_MAX_CORES))
		goto unlock_exit;
		
	core_queue = &aq->core_queues[core_id];
	
	/* Dequeue from core's circular queue (FIFO - oldest first) */
	mini_array = anneal_core_queue_dequeue(core_queue);
	if (mini_array) {
		aq->total_count--;
		
		/* Update heap */
		if (core_queue->count == 0) {
			/* Remove this core from heap */
			min_heap_pop(&aq->heap, &anneal_heap_callbacks, aq);
			WRITE_ONCE(core_queue->heap_idx, INVALID_HEAP_IDX);  /* Clear backpointer after heap operation */
		} else {
			/* Update count for this core in heap */
			top_node->count = core_queue->count;
			min_heap_sift_down(&aq->heap, 0, &anneal_heap_callbacks, aq);
		}
	} else {
		/* This shouldn't happen, but handle gracefully */
		/* Remove stale entry from heap */
		min_heap_pop(&aq->heap, &anneal_heap_callbacks, aq);
		WRITE_ONCE(core_queue->heap_idx, INVALID_HEAP_IDX);  /* Clear backpointer after heap operation */
	}
	
unlock_exit:
	spin_unlock_irqrestore(&aq->lock, flags);
	
	return mini_array;
}