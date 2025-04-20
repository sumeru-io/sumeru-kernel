/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM page_pool

#if !defined(_TRACE_PAGE_POOL_H) || defined(TRACE_HEADER_MULTI_READ)
#define      _TRACE_PAGE_POOL_H

#include <linux/types.h>
#include <linux/tracepoint.h>

#include <trace/events/mmflags.h>
#include <net/page_pool/types.h>

TRACE_EVENT(page_pool_release,

	TP_PROTO(const struct page_pool *pool,
		 s32 inflight, u32 hold, u32 release),

	TP_ARGS(pool, inflight, hold, release),

	TP_STRUCT__entry(
		__field(const struct page_pool *, pool)
		__field(s32,	inflight)
		__field(u32,	hold)
		__field(u32,	release)
		__field(u64,	cnt)
	),

	TP_fast_assign(
		__entry->pool		= pool;
		__entry->inflight	= inflight;
		__entry->hold		= hold;
		__entry->release	= release;
		__entry->cnt		= pool->destroy_cnt;
	),

	TP_printk("page_pool=%p inflight=%d hold=%u release=%u cnt=%llu",
		__entry->pool, __entry->inflight, __entry->hold,
		__entry->release, __entry->cnt)
);

TRACE_EVENT(page_pool_state_release,

	TP_PROTO(const struct page_pool *pool,
		 netmem_ref netmem, u32 release),

	TP_ARGS(pool, netmem, release),

	TP_STRUCT__entry(
		__field(const struct page_pool *,	pool)
		__field(unsigned long,			netmem)
		__field(u32,				release)
		__field(unsigned long,			pfn)
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

TRACE_EVENT(page_pool_state_hold,

	TP_PROTO(const struct page_pool *pool,
		 netmem_ref netmem, u32 hold),

	TP_ARGS(pool, netmem, hold),

	TP_STRUCT__entry(
		__field(const struct page_pool *,	pool)
		__field(unsigned long,			netmem)
		__field(u32,				hold)
		__field(unsigned long,			pfn)
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

TRACE_EVENT(page_pool_update_nid,

	TP_PROTO(const struct page_pool *pool, int new_nid),

	TP_ARGS(pool, new_nid),

	TP_STRUCT__entry(
		__field(const struct page_pool *, pool)
		__field(int,			  pool_nid)
		__field(int,			  new_nid)
	),

	TP_fast_assign(
		__entry->pool		= pool;
		__entry->pool_nid	= pool->p.nid;
		__entry->new_nid	= new_nid;
	),

	TP_printk("page_pool=%p pool_nid=%d new_nid=%d",
		  __entry->pool, __entry->pool_nid, __entry->new_nid)
);

TRACE_EVENT(page_pool_page_move,

	TP_PROTO(const struct page_pool *pool, netmem_ref netmem, u8 old_state, u8 new_state),

	TP_ARGS(pool, netmem, old_state, new_state),

	TP_STRUCT__entry(
		__field(const struct page_pool *,	pool)
		__field(unsigned long,			netmem)
		__field(u8,				old_state)
		__field(u8,				new_state)
	),

	TP_fast_assign(
		__entry->pool	= pool;
		__entry->netmem	= (__force unsigned long)netmem;
		__entry->old_state = old_state;
		__entry->new_state = new_state;
	),

	TP_printk("page_pool=%p netmem=%p old_state=%u new_state=%u",
		  __entry->pool, (void *)__entry->netmem,
		  __entry->old_state, __entry->new_state)
)

TRACE_EVENT(page_pool_pressure,

	TP_PROTO(const struct page_pool *pool, const struct sock *sk, u64 sock_id,
		 const struct sk_buff *skb, u32 used, u32 free, u32 mark),

	TP_ARGS(pool, sk, sock_id, skb, used, free, mark),

	TP_STRUCT__entry(
		__field(const struct page_pool *,	pool)
		__field(const struct sock *,		sk)
		__field(u64,				sock_id)
		__field(const struct sk_buff *,		skb)
		__field(u32,				used)
		__field(u32,				free)
		__field(u32,				mark)
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

#endif /* _TRACE_PAGE_POOL_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
