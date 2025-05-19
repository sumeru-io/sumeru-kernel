/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Single-Producer Single-Consumer (SPSC) Ring Buffer for Fixed-Size Items
 *
 * Author:
 *     Minhu Wang <minhuw@gmail.com>
 *
 * Copyright (C) 2025 Minhu Wang
 *
 * This ring buffer allows a producer to reserve a memory slot, write data
 * directly into it, and then submit it. A consumer can peek at the data
 * and then consume it. This avoids extra data copies for item passing.
 * It is designed strictly for a single producer CPU and a single consumer CPU.
 *
 */

#ifndef _LINUX_ITEM_RING_H
#define _LINUX_ITEM_RING_H 1

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/cache.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/atomic.h>
#endif

struct item_ring {
	void *buffer;
	u32 item_size;
	u32 capacity;

	struct {
		u32 idx;
		bool slot_reserved;
	} producer ____cacheline_aligned_in_smp;

	struct {
		u32 idx;
	} consumer ____cacheline_aligned_in_smp;
};

/**
 * item_spsc_ring_init - Initialize an item SPSC ring
 * @ring: The item_spsc_ring structure to initialize.
 * @n_items: The maximum number of items the ring can hold.
 * @item_size: The size of each item in bytes.
 * @gfp: GFP allocation flags for the data buffer.
 *
 * Allocates the internal data buffer. The `struct item_spsc_ring` itself
 * must be allocated by the caller.
 * Returns 0 on success, or a negative error code (e.g., -ENOMEM, -EINVAL).
 */
static inline int item_ring_init(struct item_ring *ring, u32 n_items,
				      u32 item_size, gfp_t gfp)
{
	if (unlikely(!ring || n_items == 0 || item_size == 0))
		return -EINVAL;

	ring->buffer = kvmalloc_array(n_items, item_size, gfp);
	if (!ring->buffer)
		return -ENOMEM;

	ring->item_size = item_size;
	ring->capacity = n_items;

	ring->producer.idx = 0;
	ring->producer.slot_reserved = false;
	ring->consumer.idx = 0;

	return 0;
}

/**
 * item_ring_cleanup - Free resources associated with an item SPSC ring
 * @ring: The item_spsc_ring structure to clean up (buffer previously init'd).
 *
 * Frees the internal data buffer. The `struct item_spsc_ring` itself
 * is NOT freed by this function.
 */
static inline void item_ring_cleanup(struct item_ring *ring)
{
	if (unlikely(!ring))
		return;
	kvfree(ring->buffer);
	ring->buffer = NULL; /* Help catch use-after-free */
}

/**
 * item_ring_create - Allocate and initialize an item ring
 * @n_items: The maximum number of items the ring can hold.
 * @item_size: The size of each item in bytes.
 * @gfp: GFP allocation flags for the ring structure and data buffer.
 *
 * Allocates both the `struct item_spsc_ring` and its internal data buffer.
 * Returns a pointer to the allocated ring on success, ERR_PTR on failure.
 */
static inline struct item_ring *item_ring_create(u32 n_items,
						 u32 item_size, gfp_t gfp)
{
	struct item_ring *ring;
	int ret;

	if (unlikely(n_items == 0 || item_size == 0))
		return ERR_PTR(-EINVAL);

	ring = kmalloc(sizeof(struct item_ring), gfp);
	if (!ring)
		return ERR_PTR(-ENOMEM);

	ret = item_ring_init(ring, n_items, item_size, gfp);
	if (ret) {
		kfree(ring);
		return ERR_PTR(ret);
	}
	return ring;
}

/**
 * item_spsc_ring_destroy - Free an item SPSC ring and its resources
 * @ring: The item_spsc_ring (must have been allocated by item_spsc_ring_create).
 *
 * Frees the ring structure and its internal data buffer.
 */
static inline void item_ring_destroy(struct item_ring *ring)
{
	if (IS_ERR_OR_NULL(ring))
		return;

	item_ring_cleanup(ring);
	kfree(ring);
}

/**
 * item_ring_reserve - Reserve a slot in the ring for the producer.
 * @ring: The item ring.
 *
 * For the producer ONLY.
 * Reserves a slot for writing an item. item_ring_submit() must be called
 * after writing. Only one slot can be reserved at a time by the producer.
 *
 * Returns: Pointer to the reserved memory slot on success.
 *          NULL if a slot is already reserved or if the ring is full.
 */
static inline void *item_ring_reserve(struct item_ring *ring)
{
	u32 current_producer_idx;
	u32 current_consumer_idx;

	if (unlikely(ring->producer.slot_reserved))
		return NULL;

	current_producer_idx = ring->producer.idx;
	current_consumer_idx = smp_load_acquire(&ring->consumer.idx);

	if (current_producer_idx - current_consumer_idx >= ring->capacity)
		return NULL; /* Ring is full */

	ring->producer.slot_reserved = true;

	return (char *)ring->buffer +
		   (current_producer_idx % ring->capacity) * ring->item_size;
}

/**
 * item_ring_submit - Submit a previously reserved slot.
 * @ring: The item ring.
 *
 * For the producer ONLY.
 * Makes data written to the slot (obtained via item_ring_reserve())
 * available to the consumer by advancing the producer index.
 *
 * Returns: 0 on success.
 *          -EINVAL if no slot was previously reserved.
 */
static inline int item_spsc_ring_submit(struct item_ring *ring)
{
	if (unlikely(!ring->producer.slot_reserved))
		return -EINVAL;

	ring->producer.slot_reserved = false;

	smp_wmb();

	smp_store_release(&ring->producer.idx, ring->producer.idx + 1);

	return 0;
}

/**
 * item_ring_discard - Discard a previously reserved slot.
 * @ring: The item SPSC ring.
 *
 * For the producer ONLY.
 * Call this if a slot was reserved but the producer decides not to submit data.
 *
 * Returns: 0 on success.
 *          -EINVAL if no slot was previously reserved.
 */
static inline int item_ring_discard(struct item_ring *ring)
{
	if (unlikely(!ring->producer.slot_reserved))
		return -EINVAL;

	ring->producer.slot_reserved = false;
	return 0;
}

/**
 * item_ring_peek - Peek at the next available item for the consumer.
 * @ring: The item ring.
 *
 * For the consumer ONLY.
 * Returns a pointer to the next item in the ring without consuming it.
 * The data remains valid until item_spsc_ring_consume() is called for this item.
 *
 * Returns: A pointer to the item on success.
 *          NULL if the ring is empty.
 */
static inline void *item_ring_peek(struct item_ring *ring)
{
	u32 current_producer_idx;
	u32 current_consumer_idx;

	current_consumer_idx = ring->consumer.idx;
	current_producer_idx = smp_load_acquire(&ring->producer.idx);

	if (current_consumer_idx == current_producer_idx)
		return NULL; /* Ring is empty */

	return (char *)ring->buffer +
	       (current_consumer_idx % ring->capacity) * ring->item_size;
}

/**
 * item_spsc_ring_consume - Consume the next available item.
 * @ring: The item SPSC ring.
 *
 * For the consumer ONLY.
 * Advances the consumer's read pointer, making the slot available for the producer.
 * This should be called after item_spsc_ring_peek() returned an item
 * and the consumer has finished processing it.
 *
 * Returns: 0 on success.
 *          -ENODATA if the ring was empty (caller error, ideally).
 */
static inline int item_ring_consume(struct item_ring *ring)
{
	u32 current_producer_idx;
	u32 current_consumer_idx;

	current_consumer_idx = ring->consumer.idx;
	current_producer_idx = smp_load_acquire(&ring->producer.idx);

	if (current_consumer_idx == current_producer_idx)
		return -ENODATA; /* Ring is empty */

    	barrier();

	smp_store_release(&ring->consumer.idx, ring->consumer.idx + 1);

	return 0;
}

/**
 * item_spsc_ring_items_available - Get current number of items available for consumer.
 * @ring: The item SPSC ring.
 *
 * This value can change immediately after being read in a concurrent system.
 * Useful for heuristics or approximate counts.
 * Returns: Number of items currently in the ring.
 */
static inline u32 item_ring_items_available(struct item_ring *ring)
{
    u32 prod_idx = smp_load_acquire(&ring->producer.idx);
    u32 cons_idx = smp_load_acquire(&ring->consumer.idx);

    return prod_idx - cons_idx;
}

/**
 * item_spsc_ring_space_available - Get current number of free slots for producer.
 * @ring: The item SPSC ring.
 *
 * This value can change immediately. Useful for heuristics.
 * Returns: Number of free slots currently in the ring.
 */
static inline u32 item_ring_space_available(struct item_ring *ring)
{
    u32 prod_idx = smp_load_acquire(&ring->producer.idx);
    u32 cons_idx = smp_load_acquire(&ring->consumer.idx);

    return ring->capacity - (prod_idx - cons_idx);
}

#endif /* _LINUX_ITEM_RING_H */
