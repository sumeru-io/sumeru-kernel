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
	TP_PROTO(int rq_index, int cqe_bcnt, struct page **t_pages, int cpu),
	TP_ARGS(rq_index, cqe_bcnt, t_pages, cpu),
	TP_STRUCT__entry(
		__field(int, rq_index)
		__field(int, cqe_bcnt)
		__array(struct page *, pages, 4)
		__field(int, cpu)
	),
	TP_fast_assign(
		__entry->rq_index = rq_index;
		__entry->cqe_bcnt = cqe_bcnt;
		memcpy(__entry->pages, t_pages, sizeof(struct page *) * 4);
		__entry->cpu = cpu;
	),
	TP_printk("rq_index=%d cqe_bcnt=%d pages[0]=%px pages[1]=%px pages[2]=%px pages[3]=%px cpu=%d",
		  __entry->rq_index, __entry->cqe_bcnt,
		  __entry->pages[0], __entry->pages[1],
		  __entry->pages[2], __entry->pages[3],
		  __entry->cpu)
);

TRACE_EVENT(mlx5e_cacheflow_th_skb,
	TP_PROTO(int cpu, struct sk_buff *skb),
	TP_ARGS(cpu, skb),
	TP_STRUCT__entry(
		__field(int, cpu)
		__field(int, len)
	),
	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->len = skb->len;
	),
	TP_printk("cpu=%d len=%d",
		  __entry->cpu, __entry->len)
);

#endif /* _MLX5_CACHEFLOW_TP_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ./diag
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE cacheflow_tracepoint
#include <trace/define_trace.h>
