/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mlx5

#if !defined(_MLX5_CACHEFLOW_TP_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _MLX5_CACHEFLOW_TP_H_

#include <linux/tracepoint.h>
#include <trace/events/net_probe_common.h>

#if IS_ENABLED(CONFIG_IPV6)
#define TP_CF_STORE_ADDRS(__entry, family, saddr, daddr, saddr6, daddr6)\
	do {								\
		if (family == ETH_P_IPV6) {				\
			struct in6_addr *pin6;				\
									\
			pin6 = (struct in6_addr *)__entry->saddr_v6;	\
			*pin6 = saddr6;					\
			pin6 = (struct in6_addr *)__entry->daddr_v6;	\
			*pin6 = daddr6;					\
		} else if (family == ETH_P_IP) {			\
			TP_STORE_V4MAPPED(__entry, saddr, daddr);	\
		}							\
	} while (0)
#else
#define TP_CF_STORE_ADDRS(__entry, saddr, daddr, saddr6, daddr6)	\
	TP_STORE_V4MAPPED(__entry, saddr, daddr)
#endif

TRACE_EVENT(mlx5e_mpwqe_post,

	TP_PROTO(int ix, u32 umr_completed, u32 umr_in_progress, u32 umr_missing),

	TP_ARGS(ix, umr_completed, umr_in_progress, umr_missing),

	TP_STRUCT__entry(
		__field(int, ix)
		__field(u32, umr_completed)
		__field(u32, umr_in_progress)
		__field(u32, umr_missing)
	),

	TP_fast_assign(
		__entry->ix = ix;
		__entry->umr_completed = umr_completed;
		__entry->umr_in_progress = umr_in_progress;
		__entry->umr_missing = umr_missing;
	),

	TP_printk("ix %d, umr_completed %u, umr_in_progress %u, umr_missing %u",
		  __entry->ix,
		  __entry->umr_completed,
		  __entry->umr_in_progress,
		  __entry->umr_missing)
);

TRACE_EVENT(mlx5e_wqe_post,

	TP_PROTO(int ix, u32 wqe_bulk),

	TP_ARGS(ix, wqe_bulk),

	TP_STRUCT__entry(
		__field(int, ix)
		__field(u32, wqe_bulk)
	),

	TP_fast_assign(
		__entry->ix = ix;
		__entry->wqe_bulk = wqe_bulk;
	),

	TP_printk("ix %d, wqe_bulk %u",
		  __entry->ix,
		  __entry->wqe_bulk)
);

struct arfs_tuple;

TRACE_EVENT(mlx5e_flow_rule_update,
	TP_PROTO(int flow_id, int filter_id, int rxq, struct arfs_tuple *tuple, int add),
	TP_ARGS(flow_id, filter_id, rxq, tuple, add),
	TP_STRUCT__entry(
		__field(int, flow_id)
		__field(int, filter_id)
		__field(int, rxq)
		__field(int, add)
		__field(__u16, sport)
		__field(__u16, dport)
		__field(__u16, family)
		__field(__u16, protocol)
		__array(__u8, saddr, 4)
		__array(__u8, daddr, 4)
		__array(__u8, saddr_v6, 16)
		__array(__u8, daddr_v6, 16)
	),
	TP_fast_assign(
		__entry->flow_id = flow_id;
		__entry->filter_id = filter_id;
		__entry->rxq = rxq;
		__entry->add = add;

		__entry->family = tuple->etype;
		__entry->protocol = tuple->ip_proto;
		__entry->sport = ntohs(tuple->src_port);
		__entry->dport = ntohs(tuple->dst_port);

		TP_CF_STORE_ADDRS(__entry, tuple->etype, tuple->src_ipv4, tuple->dst_ipv4,
			       tuple->src_ipv6, tuple->dst_ipv6);
	),
	TP_printk("family=%d protocol=%d sport=%u dport=%u saddr=%pI4 daddr=%pI4 saddrv6=%pI6c daddrv6=%pI6c flow_id=%d filter_id=%d rxq=%d add=%d",
		  __entry->family, __entry->protocol, __entry->sport, __entry->dport, __entry->saddr, __entry->daddr, __entry->saddr_v6, __entry->daddr_v6, __entry->flow_id, __entry->filter_id, __entry->rxq, __entry->add)
);

TRACE_EVENT(mlx5e_cacheflow_bh_cqe,
	TP_PROTO(int rq_index, int cqe_bcnt, struct page *page, int cpu),
	TP_ARGS(rq_index, cqe_bcnt, page, cpu),
	TP_STRUCT__entry(
		__field(int, rq_index)
		__field(int, cqe_bcnt)
		__field(struct page *, page)
		__field(int, cpu)
	),
	TP_fast_assign(
		__entry->rq_index = rq_index;
		__entry->cqe_bcnt = cqe_bcnt;
		__entry->page = page;
		__entry->cpu = cpu;
	),
	TP_printk("rq_index=%d cqe_bcnt=%d page=%px cpu=%d",
		  __entry->rq_index, __entry->cqe_bcnt,
		  __entry->page,
		  __entry->cpu)
);

TRACE_EVENT(mlx5e_cacheflow_th_skb,
	TP_PROTO(int cpu, struct sk_buff *skb, struct page *page),
	TP_ARGS(cpu, skb, page),
	TP_STRUCT__entry(
		__field(int, cpu)
		__field(int, len)
		__field(struct page *, page)
	),
	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->len = skb->len;
		__entry->page = page;
	),
	TP_printk("cpu=%d len=%d page=%px",
		  __entry->cpu, __entry->len,
		  __entry->page)
);

TRACE_EVENT(mlx5e_cacheflow_th_ipi_scheduled,
	TP_PROTO(int cpu, u64 current_time, u64 last_scheduled_time, u64 cqe_fifo_len),
	TP_ARGS(cpu, current_time, last_scheduled_time, cqe_fifo_len),
	TP_STRUCT__entry(
		__field(int, cpu)
		__field(u64, current_time)
		__field(u64, last_scheduled_time)
		__field(u64, cqe_fifo_len)
		__field(u64, schedule_delay)
	),
	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->current_time = current_time;
		__entry->last_scheduled_time = last_scheduled_time;
		__entry->cqe_fifo_len = cqe_fifo_len;
		__entry->schedule_delay = current_time - last_scheduled_time;
	),
	TP_printk("cpu=%d current_time=%llu last_scheduled_time=%llu schedule_delay=%llu cqe_fifo_len=%llu",
		  __entry->cpu, __entry->current_time, __entry->last_scheduled_time, __entry->schedule_delay, __entry->cqe_fifo_len)
);

TRACE_EVENT(mlx5e_cacheflow_th_ipi_raised,
	TP_PROTO(int cpu, int work_done, int budget, int cqe_fifo_len),
	TP_ARGS(cpu, work_done, budget, cqe_fifo_len),
	TP_STRUCT__entry(
		__field(int, cpu)
		__field(int, work_done)
		__field(int, budget)
		__field(int, cqe_fifo_len)
	),
	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->work_done = work_done;
		__entry->budget = budget;
		__entry->cqe_fifo_len = cqe_fifo_len;
	),
	TP_printk("cpu=%d work_done=%d budget=%d cqe_fifo_len=%d",
		  __entry->cpu, __entry->work_done, __entry->budget, __entry->cqe_fifo_len)
);

#endif /* _MLX5_CACHEFLOW_TP_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ./diag
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE cacheflow_tracepoint
#include <trace/define_trace.h>
