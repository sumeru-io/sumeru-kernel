/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cacheflow

#if !defined(_TRACE_CACHEFLOW_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_CACHEFLOW_H

#include <linux/tracepoint.h>
#include <linux/netdevice.h>
#include <linux/tcp.h>

extern int cacheflow_alpha;

TRACE_EVENT(cacheflow_napi_poll,

	    TP_PROTO(struct net_device *dev, int core, int work_done),
	    TP_ARGS(dev, core, work_done),

	    TP_STRUCT__entry(
		    __array(char, dev_name, IFNAMSIZ)
		    __field(int, core)
		    __field(int, work_done)
	    ),

	    TP_fast_assign(
		    strscpy(__entry->dev_name,
			    dev ? dev->name : "unknown", IFNAMSIZ);
		    __entry->core = core;
		    __entry->work_done = work_done;
	    ),

	    TP_printk("dev=%s core=%d work_done=%d",
		      __entry->dev_name,
		      __entry->core,
		      __entry->work_done
	    )
);

TRACE_EVENT(
	cacheflow_page_pool_page_move,

	TP_PROTO(const struct cacheflow_page_pool *pool, netmem_ref netmem,
		 u8 old_state, u8 new_state, u32 alloc_pages,
		 u32 cache_pages),

	TP_ARGS(pool, netmem, old_state, new_state, alloc_pages,
		cache_pages),

	TP_STRUCT__entry(
		__field(const struct cacheflow_page_pool *, pool)
		__field(unsigned long, netmem)
		__field(u8, old_state)
		__field(u8, new_state)
		__field(u32, alloc_pages)
		__field(u32, cache_pages)
	),

	TP_fast_assign(
		__entry->pool = pool;
		__entry->netmem = (__force unsigned long)netmem;
		__entry->old_state = old_state;
		__entry->new_state = new_state;
		__entry->alloc_pages = alloc_pages;
		__entry->cache_pages = cache_pages;
	),

	TP_printk(
		"page_pool=%p netmem=%p old_state=%u new_state=%u alloc_pages=%u cache_pages=%u",
		__entry->pool, (void *)__entry->netmem, __entry->old_state,
		__entry->new_state, __entry->alloc_pages, __entry->cache_pages
	)
)

TRACE_EVENT(cacheflow_page_pool_state_hold,

	    TP_PROTO(const struct cacheflow_page_pool *pool, netmem_ref netmem,
		     u32 hold),

	    TP_ARGS(pool, netmem, hold),

	    TP_STRUCT__entry(
		    __field(const struct cacheflow_page_pool *, pool)
		    __field(unsigned long, netmem)
		    __field(u32, hold)
		    __field(unsigned long, pfn)
	    ),

	    TP_fast_assign(
		    __entry->pool = pool;
		    __entry->netmem = (__force unsigned long)netmem;
		    __entry->hold = hold;
		    __entry->pfn = netmem_pfn_trace(netmem);
	    ),

	    TP_printk("page_pool=%p netmem=%p is_net_iov=%lu, pfn=0x%lx hold=%u",
		      __entry->pool, (void *)__entry->netmem,
		      __entry->netmem & NET_IOV, __entry->pfn, __entry->hold
	    )
);

TRACE_EVENT(
	cacheflow_page_pool_state_release,

	TP_PROTO(const struct cacheflow_page_pool *pool, netmem_ref netmem,
		 u32 release),

	TP_ARGS(pool, netmem, release),

	TP_STRUCT__entry(
		__field(const struct cacheflow_page_pool *, pool)
		__field(unsigned long, netmem)
		__field(u32, release)
		__field(unsigned long, pfn)
	),

	TP_fast_assign(
		__entry->pool = pool;
		__entry->netmem = (__force unsigned long)netmem;
		__entry->release = release;
		__entry->pfn = netmem_pfn_trace(netmem);
	),

	TP_printk("page_pool=%p netmem=%p is_net_iov=%lu pfn=0x%lx release=%u",
		  __entry->pool, (void *)__entry->netmem,
		  __entry->netmem & NET_IOV, __entry->pfn, __entry->release
	)
);

TRACE_EVENT(
	cacheflow_anneal_queue_enqueue,

	TP_PROTO(void *aq, void *mini_array, u16 core_id, u32 queue_depth, u32 total_depth),

	TP_ARGS(aq, mini_array, core_id, queue_depth, total_depth),

	TP_STRUCT__entry(
		__field(void *, aq)
		__field(void *, mini_array)
		__field(u16, core_id)
		__field(u32, queue_depth)
		__field(u32, total_depth)
	),

	TP_fast_assign(
		__entry->aq = aq;
		__entry->mini_array = mini_array;
		__entry->core_id = core_id;
		__entry->queue_depth = queue_depth;
		__entry->total_depth = total_depth;
	),

	TP_printk("anneal_queue=%p mini_array=%p core_id=%u queue_depth=%u total_depth=%u",
		__entry->aq, __entry->mini_array, __entry->core_id,
		__entry->queue_depth, __entry->total_depth
	)
);

TRACE_EVENT(
	cacheflow_anneal_queue_dequeue,

	TP_PROTO(void *aq, void *mini_array, u16 core_id, u32 queue_depth, u32 total_depth),

	TP_ARGS(aq, mini_array, core_id, queue_depth, total_depth),

	TP_STRUCT__entry(
		__field(void *, aq)
		__field(void *, mini_array)
		__field(u16, core_id)
		__field(u32, queue_depth)
		__field(u32, total_depth)
	),

	TP_fast_assign(
		__entry->aq = aq;
		__entry->mini_array = mini_array;
		__entry->core_id = core_id;
		__entry->queue_depth = queue_depth;
		__entry->total_depth = total_depth;
	),

	TP_printk("anneal_queue=%p mini_array=%p core_id=%u queue_depth=%u total_depth=%u",
		__entry->aq, __entry->mini_array, __entry->core_id,
		__entry->queue_depth, __entry->total_depth
	)
);

TRACE_EVENT(
	cacheflow_rate_est,

	TP_PROTO(const struct sock *sk, u64 sock_cookie),

	TP_ARGS(sk, sock_cookie),

	TP_STRUCT__entry(
		__field(const struct sock *, sk)
		__field(u64, sock_cookie)
		__field(u32, rcv_rtt)
		__field(u32, smooth_recv_rate)
		__field(u32, latest_recv_rate)
		__field(u32, smooth_copied_rate)
		__field(u32, latest_copied_rate)
		__field(u32, smooth_ack_rate)
		__field(u32, latest_ack_rate)
	),

	TP_fast_assign(
		const struct tcp_sock *tp = tcp_sk(sk);
		__entry->sk = sk;
		__entry->sock_cookie = sock_cookie;
		__entry->rcv_rtt = tp->rcv_rtt_est.rtt_us >> cacheflow_alpha;
		__entry->smooth_recv_rate = tp->rcv_rate_est.recv_rate >> cacheflow_alpha;
		__entry->latest_recv_rate = tp->rcv_rate_est.latest_recv_rate;
		__entry->smooth_copied_rate = tp->rcv_rate_est.copied_rate >> cacheflow_alpha;
		__entry->latest_copied_rate = tp->rcv_rate_est.latest_copied_rate;
		__entry->smooth_ack_rate = tp->rcv_rate_est.ack_rate >> cacheflow_alpha;
		__entry->latest_ack_rate = tp->rcv_rate_est.latest_ack_rate;
	),

	TP_printk(
		"sk=%p sock_cookie=%llu rcv_rtt=%u smooth_recv_rate=%u latest_recv_rate=%u smooth_copied_rate=%u latest_copied_rate=%u smooth_ack_rate=%u latest_ack_rate=%u",
		__entry->sk, __entry->sock_cookie, __entry->rcv_rtt, __entry->smooth_recv_rate, __entry->latest_recv_rate,
		__entry->smooth_copied_rate, __entry->latest_copied_rate, __entry->smooth_ack_rate, __entry->latest_ack_rate
	)
);

TRACE_EVENT(
	cacheflow_rcv_rtt_update,

	TP_PROTO(const struct sock *sk, u64 sock_cookie, u64 window_size, u64 rcv_rtt, u64 interval, bool ts_based),

	TP_ARGS(sk, sock_cookie, window_size, rcv_rtt, interval, ts_based),

	TP_STRUCT__entry(
		__field(const struct sock *, sk)
		__field(u64, sock_cookie)
		__field(u64, window_size)
		__field(u64, rcv_rtt)
		__field(u64, interval)
		__field(bool, ts_based)
	),

	TP_fast_assign(
		__entry->sk = sk;
		__entry->sock_cookie = sock_cookie;
		__entry->window_size = window_size;
		__entry->rcv_rtt = rcv_rtt;
		__entry->interval = interval;
		__entry->ts_based = ts_based;
	),

	TP_printk(
		"sk=%p sock_cookie=%llu window_size=%llu rcv_rtt=%llu interval=%llu ts_based=%d",
		__entry->sk, __entry->sock_cookie, __entry->window_size, __entry->rcv_rtt, __entry->interval,
		__entry->ts_based
	)
);

TRACE_EVENT(
	cacheflow_mark,

	TP_PROTO(u64 sock_cookie, u32 cacheflow_cookie, u32 allocated_pages, u32 free_pages, u32 thresh, u32 recv_qlen,
		 u32 backlog_qlen, u32 rtt, struct tcp_sock* tp, int mark),

	TP_ARGS(sock_cookie, cacheflow_cookie, allocated_pages, free_pages, thresh, recv_qlen, backlog_qlen, rtt, tp, mark),

	TP_STRUCT__entry(
		__field(u64, sock_cookie)
		__field(u32, cacheflow_cookie)
		__field(u32, allocated_pages)
		__field(u32, free_pages)
		__field(u32, thresh)
		__field(u32, recv_qlen)
		__field(u32, backlog_qlen)
		__field(u32, rtt)
		__field(u32, smooth_recv_rate)
		__field(u32, latest_recv_rate)
		__field(u32, smooth_copied_rate)
		__field(u32, latest_copied_rate)
		__field(u32, smooth_ack_rate)
		__field(u32, latest_ack_rate)
		__field(int, mark)
	),

	TP_fast_assign(
		__entry->sock_cookie = sock_cookie;
		__entry->cacheflow_cookie = cacheflow_cookie;
		__entry->allocated_pages = allocated_pages;
		__entry->free_pages = free_pages;
		__entry->thresh = thresh;
		__entry->recv_qlen = recv_qlen;
		__entry->backlog_qlen = backlog_qlen;
		__entry->rtt = rtt;
		__entry->smooth_recv_rate = tp->rcv_rate_est.recv_rate >> cacheflow_alpha;
		__entry->latest_recv_rate = tp->rcv_rate_est.latest_recv_rate;
		__entry->smooth_copied_rate = tp->rcv_rate_est.copied_rate >> cacheflow_alpha;
		__entry->latest_copied_rate = tp->rcv_rate_est.latest_copied_rate;
		__entry->smooth_ack_rate = tp->rcv_rate_est.ack_rate >> cacheflow_alpha;
		__entry->latest_ack_rate = tp->rcv_rate_est.latest_ack_rate;
		__entry->mark = mark;
	),

	TP_printk(
		"sock_cookie=%llu cacheflow_cookie=%u allocated_pages=%u free_pages=%u thresh=%u recv_qlen=%u backlog_qlen=%u rtt=%u smooth_recv_rate=%u latest_recv_rate=%u smooth_copied_rate=%u latest_copied_rate=%u smooth_ack_rate=%u latest_ack_rate=%u mark=%d",
		__entry->sock_cookie, __entry->cacheflow_cookie, __entry->allocated_pages, __entry->free_pages, __entry->thresh,
		__entry->recv_qlen, __entry->backlog_qlen, __entry->rtt,
		__entry->smooth_recv_rate, __entry->latest_recv_rate, __entry->smooth_copied_rate, __entry->latest_copied_rate,
		__entry->smooth_ack_rate, __entry->latest_ack_rate, __entry->mark
	)
);

TRACE_EVENT(
	cacheflow_schedule_priority,

	TP_PROTO(u64 sock_cookie, u32 cacheflow_cookie, u32 recv_qlen, u32 backlog_qlen, u32 pid, int priority),

	TP_ARGS(sock_cookie, cacheflow_cookie, recv_qlen, backlog_qlen, pid, priority),

	TP_STRUCT__entry(
		__field(u64, sock_cookie)
		__field(u32, cacheflow_cookie)
		__field(u32, recv_qlen)
		__field(u32, backlog_qlen)
		__field(u32, pid)
		__field(int, priority)
	),

	TP_fast_assign(
		__entry->sock_cookie = sock_cookie;
		__entry->cacheflow_cookie = cacheflow_cookie;
		__entry->recv_qlen = recv_qlen;
		__entry->backlog_qlen = backlog_qlen;
		__entry->pid = pid;
		__entry->priority = priority;
	),

	TP_printk(
		"sock_cookie=%llu cacheflow_cookie=%u recv_qlen=%u backlog_qlen=%u pid=%u priority=%d",
		__entry->sock_cookie, __entry->cacheflow_cookie, __entry->recv_qlen, __entry->backlog_qlen, __entry->pid, __entry->priority
	)
)

TRACE_EVENT(
	cacheflow_queue_depth,

	TP_PROTO(u32 qlen),

	TP_ARGS(qlen),

	TP_STRUCT__entry(
		__field(u32, qlen)
	),

	TP_fast_assign(
		__entry->qlen = qlen;
	),

	TP_printk(
		"qlen=%u",
		__entry->qlen
	)
);

TRACE_EVENT(ddsketch_add,
	TP_PROTO(u64 trace_id, u32 value, s32 index, u16 bucket_idx, u32 bucket_count, u64 total_count),
	TP_ARGS(trace_id, value, index, bucket_idx, bucket_count, total_count),
	TP_STRUCT__entry(
		__field(u64, trace_id)
		__field(u32, value)
		__field(s32, index)
		__field(u16, bucket_idx)
		__field(u32, bucket_count)
		__field(u64, total_count)
	),
	TP_fast_assign(
		__entry->trace_id = trace_id;
		__entry->value = value;
		__entry->index = index;
		__entry->bucket_idx = bucket_idx;
		__entry->bucket_count = bucket_count;
		__entry->total_count = total_count;
	),
	TP_printk("trace_id=%llu value=%u index=%d bucket_idx=%u bucket_count=%u total_count=%llu",
		  __entry->trace_id,
		  __entry->value,
		  __entry->index,
		  __entry->bucket_idx,
		  __entry->bucket_count,
		  __entry->total_count)
);

TRACE_EVENT(ddsketch_percentile,
	TP_PROTO(u64 trace_id, u32 percentile, u64 target_rank, u32 result, u16 num_buckets, u64 total_count),
	TP_ARGS(trace_id, percentile, target_rank, result, num_buckets, total_count),
	TP_STRUCT__entry(
		__field(u64, trace_id)
		__field(u32, percentile)
		__field(u64, target_rank)
		__field(u32, result)
		__field(u16, num_buckets)
		__field(u64, total_count)
	),
	TP_fast_assign(
		__entry->trace_id = trace_id;
		__entry->percentile = percentile;
		__entry->target_rank = target_rank;
		__entry->result = result;
		__entry->num_buckets = num_buckets;
		__entry->total_count = total_count;
	),
	TP_printk("trace_id=%llu p%u.%u target_rank=%llu result=%u buckets=%u total=%llu",
		  __entry->trace_id,
		  __entry->percentile / 10,
		  __entry->percentile % 10,
		  __entry->target_rank,
		  __entry->result,
		  __entry->num_buckets,
		  __entry->total_count)
);

TRACE_EVENT(
	cacheflow_cache_state,

	TP_PROTO(u32 buffer_usage, u32 cos_id, u32 old_ways, u32 new_ways),

	TP_ARGS(buffer_usage, cos_id, old_ways, new_ways),

	TP_STRUCT__entry(
		__field(u32, buffer_usage)
		__field(u32, cos_id)
		__field(u32, old_ways)
		__field(u32, new_ways)
	),

	TP_fast_assign(
		__entry->buffer_usage = buffer_usage;
		__entry->cos_id = cos_id;
		__entry->old_ways = old_ways;
		__entry->new_ways = new_ways;
	),

	TP_printk("buffer_usage=%u cos=%u cache_ways=%u->%u",
		  __entry->buffer_usage, __entry->cos_id, __entry->old_ways,
		  __entry->new_ways)
);

TRACE_EVENT(
	cacheflow_cache_boost_decision,

	TP_PROTO(const struct cacheflow_page_pool *pool, u32 allocated_pages,
		 u32 current_ways, u32 max_ways, u32 min_ways,
		 u32 estimated_usage_kb, u32 current_cache_kb, int decision),

	TP_ARGS(pool, allocated_pages, current_ways, max_ways, min_ways,
		estimated_usage_kb, current_cache_kb, decision),

	TP_STRUCT__entry(
		__field(const struct cacheflow_page_pool *, pool)
		__field(u32, allocated_pages)
		__field(u32, current_ways)
		__field(u32, max_ways)
		__field(u32, min_ways)
		__field(u32, estimated_usage_kb)
		__field(u32, current_cache_kb)
		__field(int, decision)
	),

	TP_fast_assign(
		__entry->pool = pool;
		__entry->allocated_pages = allocated_pages;
		__entry->current_ways = current_ways;
		__entry->max_ways = max_ways;
		__entry->min_ways = min_ways;
		__entry->estimated_usage_kb = estimated_usage_kb;
		__entry->current_cache_kb = current_cache_kb;
		__entry->decision = decision;
	),

	TP_printk("pool=%p allocated_pages=%u current_ways=%u max_ways=%u min_ways=%u estimated_usage_kb=%u current_cache_kb=%u decision=%s",
		  __entry->pool, __entry->allocated_pages, __entry->current_ways,
		  __entry->max_ways, __entry->min_ways, __entry->estimated_usage_kb,
		  __entry->current_cache_kb,
		  __entry->decision == 1 ? "BOOST" :
		  __entry->decision == -1 ? "SHRINK" : "KEEP")
);


#endif /* _TRACE_CACHEFLOW_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
