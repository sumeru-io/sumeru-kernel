/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MLX5_EN_CACHEFLOW_RQ_TRACKER_H__
#define __MLX5_EN_CACHEFLOW_RQ_TRACKER_H__

#include <linux/ktime.h>
#include <linux/item_deque.h>
#include <trace/events/skb.h>

struct mlx5e_cacheflow_rq_tracker_entry {
	ktime_t received;
	ktime_t processed;
};

struct mlx5e_cacheflow_rq_tracker {
	struct item_deque *history;
	ssize_t size;
	ktime_t monitor_start;
	ssize_t monitor_total;
	ssize_t monitor_n;
	u64	cacheflow_id;
};

static inline int
mlx5e_cacheflow_rq_tracker_update(struct mlx5e_cacheflow_rq_tracker *tracker,
				  ktime_t processed, ktime_t received)
{
	struct mlx5e_cacheflow_rq_tracker_entry *entry;

	while ((entry = item_deque_front(tracker->history))) {
		if (ktime_after(received, entry->processed))
			item_deque_pop_front(tracker->history);
		else
			break;
	}
	entry = item_deque_peek_back(tracker->history);
	entry->processed = processed;
	entry->received = received;
	item_deque_push_back(tracker->history);

	tracker->size = item_deque_size(tracker->history);

	tracker->cacheflow_id++;

	trace_skb_ring_timestamp(tracker->cacheflow_id, 0,
		received, processed);

	return tracker->cacheflow_id;
}

static inline struct mlx5e_cacheflow_rq_tracker *
mlx5e_cacheflow_rq_tracker_create(ssize_t size)
{
	struct mlx5e_cacheflow_rq_tracker *tracker =
		kvzalloc(sizeof(*tracker), GFP_KERNEL);

	if (!tracker)
		return NULL;

	tracker->history = item_deque_create(
		size, sizeof(struct mlx5e_cacheflow_rq_tracker_entry),
		GFP_KERNEL);

	tracker->cacheflow_id = 0;

	if (!tracker->history) {
		kvfree(tracker);
		return NULL;
	}

	return tracker;
}

static inline void
mlx5e_cacheflow_rq_tracker_destroy(struct mlx5e_cacheflow_rq_tracker *tracker)
{
	item_deque_destroy(tracker->history);
	kvfree(tracker);
}

#endif /* __MLX5_EN_CACHEFLOW_RQ_TRACKER_H__ */
