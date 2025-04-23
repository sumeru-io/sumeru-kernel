/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_H
#define __CACHEFLOW_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/skbuff.h>

#include <net/page_pool/helpers.h>

struct page_pool_mem_usage {
	struct page_pool *pool;
	u32 used_pages;
	u32 free_pages;
};

extern u8 cacheflow_mark_enable;
extern u8 cacheflow_track_enable;
extern int cacheflow_buffer_size;
extern int cacheflow_thresh;

static inline bool is_cacheflow_track_enabled(void)
{
	return READ_ONCE(cacheflow_track_enable) > 0;
}

static inline bool is_cacheflow_mark_enabled(void)
{
	return READ_ONCE(cacheflow_mark_enable) > 0;
}

static inline int get_cacheflow_pool_size(void) {
	return READ_ONCE(cacheflow_buffer_size);
}

int skb_pp_pressure(struct sk_buff *skb, struct page_pool_mem_usage *usage);

#endif /* __CACHEFLOW_H */
