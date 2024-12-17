/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_H
#define __CACHEFLOW_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/skbuff.h>

extern u8 cacheflow_enable;

static inline bool is_cacheflow_enabled(void)
{
	return READ_ONCE(cacheflow_enable) > 0;
}

static inline int page_pool_alloc_allowed(void)
{
	return READ_ONCE(cacheflow_enable) != 1;
}

static inline int skb_with_pressure(const struct sk_buff *skb)
{
#ifdef CONFIG_NET_CACHEFLOW
	int i;
	struct page* page;
	if (!skb->pp_recycle)
		return 0;

	if (skb->head_frag && virt_to_page(skb->head)->pp_pressure)
		return 1;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		page = netmem_to_page(skb_shinfo(skb)->frags[i].netmem);
		if (page->pp_pressure)
			return 1;
	}
#endif
	return 0;
}

#endif /* __CACHEFLOW_H */
