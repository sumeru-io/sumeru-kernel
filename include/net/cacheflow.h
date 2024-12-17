/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_H
#define __CACHEFLOW_H

#include <linux/types.h>
#include <linux/compiler.h>

extern u8 cacheflow_enable;

static inline bool is_cacheflow_enabled(void)
{
	return READ_ONCE(cacheflow_enable) > 0;
}

static inline int page_pool_alloc_allowed(void)
{
	return READ_ONCE(cacheflow_enable) != 1;
}
#endif /* __CACHEFLOW_H */
