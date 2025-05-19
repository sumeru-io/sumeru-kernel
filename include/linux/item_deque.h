/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Double-Ended Queue (Deque) for Fixed-Size Items
 *
 * Author: Minhu Wang <minhuw@acm.org>
 *
 * This deque allows pushing and popping items at both ends (front/back).
 *
 * Usage pattern for push:
 *   void *slot = item_deque_peek_back(&dq);
 *   if (slot) { ... write to *slot ...; item_deque_push_back(&dq); }
 *
 *   void *slot = item_deque_peek_front(&dq);
 *   if (slot) { ... write to *slot ...; item_deque_push_front(&dq); }
 */

#ifndef _LINUX_ITEM_DEQUE_H
#define _LINUX_ITEM_DEQUE_H 1

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>

struct item_deque {
	void *buffer;
	u32 item_size;
	u32 capacity; // size of the circular buffer; deque can hold capacity - 1 items
	u32 head; // index of first element (front)
	u32 tail; // index one past last element (back)
};

/**
 * item_deque_init - Initialize a deque.
 * @dq: The deque to initialize.
 * @n_items: The number of item slots to allocate in the buffer.
 *           Note: The deque can hold a maximum of (n_items - 1) items.
 * @item_size: The size of each item in bytes.
 * @gfp: The GFP mask for allocation.
 *
 * Returns 0 on success, -EINVAL for invalid arguments, or -ENOMEM if
 * memory allocation fails.
 */
static inline int item_deque_init(struct item_deque *dq, u32 n_items, u32 item_size, gfp_t gfp)
{
	if (!dq || n_items == 0 || item_size == 0)
		return -EINVAL;

	dq->buffer = kvmalloc_array(n_items, item_size, gfp);
	if (!dq->buffer)
		return -ENOMEM;
	dq->item_size = item_size;
	dq->capacity = n_items;
	dq->head = 0;
	dq->tail = 0;
	return 0;
}

static inline struct item_deque* item_deque_create(u32 n_items, u32 item_size, gfp_t gfp)
{
	if (n_items == 0 || item_size == 0)
		return NULL;

	struct item_deque *dq = kzalloc(sizeof(struct item_deque), gfp);
	if (!dq)
		return NULL;

	item_deque_init(dq, n_items, item_size, gfp);
	return dq;
}

static inline void item_deque_cleanup(struct item_deque *dq)
{
	if (!dq)
		return;
	kvfree(dq->buffer);
	dq->buffer = NULL;
	dq->item_size = 0;
	dq->capacity = 0;
	dq->head = 0;
	dq->tail = 0;
}

static inline void item_deque_destroy(struct item_deque *dq)
{
	if (!dq)
		return;
	item_deque_cleanup(dq);
	kfree(dq);
}

static inline bool item_deque_is_empty(const struct item_deque *dq)
{
	return dq->head == dq->tail;
}

static inline bool item_deque_is_full(const struct item_deque *dq)
{
	return ((dq->tail + 1) % dq->capacity) == dq->head;
}

/*
 * Reserve a slot at the back for in-place construction. Returns pointer to slot, or NULL if full.
 */
static inline void *item_deque_peek_back(struct item_deque *dq)
{
	if (item_deque_is_full(dq))
		return NULL;
	return (char *)dq->buffer + dq->tail * dq->item_size;
}

/*
 * After writing to the slot returned by item_deque_peek_back, call this to advance the tail.
 * Returns true if successful, false if full (should only be called after a successful peek).
 */
static inline bool item_deque_push_back(struct item_deque *dq)
{
	if (item_deque_is_full(dq))
		return false;
	dq->tail = (dq->tail + 1) % dq->capacity;
	return true;
}

/*
 * Reserve a slot at the front for in-place construction. Returns pointer to slot, or NULL if full.
 */
static inline void *item_deque_peek_front(struct item_deque *dq)
{
	u32 new_head;
	if (item_deque_is_full(dq))
		return NULL;
	new_head = (dq->head + dq->capacity - 1) % dq->capacity;
	return (char *)dq->buffer + new_head * dq->item_size;
}

/*
 * After writing to the slot returned by item_deque_peek_front, call this to advance the head.
 * Returns true if successful, false if full (should only be called after a successful peek).
 */
static inline bool item_deque_push_front(struct item_deque *dq)
{
	if (item_deque_is_full(dq))
		return false;
	dq->head = (dq->head + dq->capacity - 1) % dq->capacity;
	return true;
}

/*
 * Remove the front item (first element) from the deque, C++ style.
 * Returns true if an item was removed, false if the deque was empty.
 */
static inline bool item_deque_pop_front(struct item_deque *dq)
{
	if (item_deque_is_empty(dq))
		return false;
	dq->head = (dq->head + 1) % dq->capacity;
	return true;
}

/*
 * Remove the back item (last element) from the deque, C++ style.
 * Returns true if an item was removed, false if the deque was empty.
 */
static inline bool item_deque_pop_back(struct item_deque *dq)
{
	if (item_deque_is_empty(dq))
		return false;
	dq->tail = (dq->tail + dq->capacity - 1) % dq->capacity;
	return true;
}

/*
 * Return a pointer to the front item (first element), or NULL if empty.
 * The returned pointer is valid until the next modification to the deque.
 */
static inline void *item_deque_front(const struct item_deque *dq)
{
	if (item_deque_is_empty(dq))
		return NULL;
	return (char *)dq->buffer + dq->head * dq->item_size;
}

/*
 * Return a pointer to the back item (last element), or NULL if empty.
 * The returned pointer is valid until the next modification to the deque.
 */
static inline void *item_deque_back(const struct item_deque *dq)
{
	if (item_deque_is_empty(dq))
		return NULL;
	return (char *)dq->buffer + ((dq->tail + dq->capacity - 1) % dq->capacity) * dq->item_size;
}

/*
 * Returns the number of items currently in the deque.
 */
static inline u32 item_deque_size(const struct item_deque *dq)
{
	return (dq->tail - dq->head + dq->capacity) % dq->capacity;
}

#endif /* _LINUX_ITEM_DEQUE_H */ 