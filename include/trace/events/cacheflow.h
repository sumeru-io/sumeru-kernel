/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cacheflow

#if !defined(_TRACE_CACHEFLOW_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_CACHEFLOW_H

#include <linux/tracepoint.h>
#include <linux/netdevice.h>

TRACE_EVENT(cacheflow_napi_poll,

	TP_PROTO(struct net_device *dev, int core, int work_done),
	TP_ARGS(dev, core, work_done),

	TP_STRUCT__entry(
		__array(char, dev_name, IFNAMSIZ)
		__field(int, core)
		__field(int, work_done)
	),

	TP_fast_assign(
		strscpy(__entry->dev_name, dev ? dev->name : "unknown", IFNAMSIZ);
		__entry->core = core;
		__entry->work_done = work_done;
	),

	TP_printk("dev=%s core=%d work_done=%d", __entry->dev_name, __entry->core, __entry->work_done)
);

TRACE_EVENT(cacheflow_page_pool_page_move,

	TP_PROTO(const struct cacheflow_page_pool *pool, netmem_ref netmem,
		 u8 old_state, u8 new_state, u32 alloc_pages,
		 u32 array_pages, u32 ring_pages),

	TP_ARGS(pool, netmem, old_state, new_state, alloc_pages, array_pages, ring_pages),

	TP_STRUCT__entry(
		__field(const struct cacheflow_page_pool *,	pool)
		__field(unsigned long,				netmem)
		__field(u8,					old_state)
		__field(u8,					new_state)
		__field(u32,					alloc_pages)
		__field(u32,					array_pages)
		__field(u32,					ring_pages)
	),

	TP_fast_assign(
		__entry->pool	= pool;
		__entry->netmem	= (__force unsigned long)netmem;
		__entry->old_state = old_state;
		__entry->new_state = new_state;
		__entry->alloc_pages = alloc_pages;
		__entry->array_pages = array_pages;
		__entry->ring_pages = ring_pages;
	),

	TP_printk("page_pool=%p netmem=%p old_state=%u new_state=%u alloc_pages=%u array_pages=%u ring_pages=%u",
		  __entry->pool, (void *)__entry->netmem,
		  __entry->old_state, __entry->new_state, __entry->alloc_pages, __entry->array_pages, __entry->ring_pages)
)

TRACE_EVENT(cacheflow_page_pool_pressure,

	TP_PROTO(const struct cacheflow_page_pool *pool, const struct sock *sk, u64 sock_id,
		 const struct sk_buff *skb, u32 used, u32 free, u32 mark),

	TP_ARGS(pool, sk, sock_id, skb, used, free, mark),

	TP_STRUCT__entry(
		__field(const struct cacheflow_page_pool *,	pool)
		__field(const struct sock *,			sk)
		__field(u64,					sock_id)
		__field(const struct sk_buff *,			skb)
		__field(u32,					used)
		__field(u32,					free)
		__field(u32,					mark)
	),

	TP_fast_assign(
		__entry->pool		= pool;
		__entry->sk		= sk;
		__entry->sock_id 	= sock_id;
		__entry->skb		= skb;
		__entry->used		= used;
		__entry->free		= free;
		__entry->mark		= mark;
	),

	TP_printk("page_pool=%p sk=%p sock_id=%llu skb=%p used=%u free=%u mark=%u",
		  __entry->pool, __entry->sk, __entry->sock_id, __entry->skb,
		  __entry->used, __entry->free, __entry->mark)
)

TRACE_EVENT(cacheflow_page_pool_state_hold,

	TP_PROTO(const struct cacheflow_page_pool *pool,
		 netmem_ref netmem, u32 hold),

	TP_ARGS(pool, netmem, hold),

	TP_STRUCT__entry(
		__field(const struct cacheflow_page_pool *,	pool)
		__field(unsigned long,				netmem)
		__field(u32,					hold)
		__field(unsigned long,				pfn)
	),

	TP_fast_assign(
		__entry->pool	= pool;
		__entry->netmem	= (__force unsigned long)netmem;
		__entry->hold	= hold;
		__entry->pfn	= netmem_pfn_trace(netmem);
	),

	TP_printk("page_pool=%p netmem=%p is_net_iov=%lu, pfn=0x%lx hold=%u",
		  __entry->pool, (void *)__entry->netmem,
		  __entry->netmem & NET_IOV, __entry->pfn, __entry->hold)
);

TRACE_EVENT(cacheflow_page_pool_state_release,

	TP_PROTO(const struct cacheflow_page_pool *pool,
		 netmem_ref netmem, u32 release),

	TP_ARGS(pool, netmem, release),

	TP_STRUCT__entry(
		__field(const struct cacheflow_page_pool *,	pool)
		__field(unsigned long,				netmem)
		__field(u32,					release)
		__field(unsigned long,				pfn)
	),

	TP_fast_assign(
		__entry->pool		= pool;
		__entry->netmem		= (__force unsigned long)netmem;
		__entry->release	= release;
		__entry->pfn		= netmem_pfn_trace(netmem);
	),

	TP_printk("page_pool=%p netmem=%p is_net_iov=%lu pfn=0x%lx release=%u",
		  __entry->pool, (void *)__entry->netmem,
		  __entry->netmem & NET_IOV, __entry->pfn, __entry->release)
);
#endif /* _TRACE_CACHEFLOW_H */

/* This part must be outside protection */
#include <trace/define_trace.h>