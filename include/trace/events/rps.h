/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM rps

#if !defined(_TRACE_RPS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RPS_H

#include <net/ipv6.h>
#include <linux/tracepoint.h>
#include <linux/ipv6.h>
#include <trace/events/net_probe_common.h>


TRACE_EVENT(sk_rps_core_change,

	TP_PROTO(const struct sock *sk, u32 hash, int new_cpu),

	TP_ARGS(sk, hash, new_cpu),

	TP_STRUCT__entry(
		__field(const void *, skaddr)
		__field(u32, hash)
		__field(int, cpu)
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
		const struct inet_sock *inet = inet_sk(sk);
		__be32 *p32;

		__entry->skaddr = sk;
		__entry->hash = hash;
		__entry->cpu = new_cpu;
		__entry->family = sk->sk_family;
		__entry->protocol = sk->sk_protocol;
		__entry->sport = ntohs(inet->inet_sport);
		__entry->dport = ntohs(inet->inet_dport);

		p32 = (__be32 *) __entry->saddr;
		*p32 = inet->inet_saddr;

		p32 = (__be32 *) __entry->daddr;
		*p32 =  inet->inet_daddr;

		TP_STORE_ADDRS(__entry, inet->inet_saddr, inet->inet_daddr,
			       sk->sk_v6_rcv_saddr, sk->sk_v6_daddr);
	),

	TP_printk("family=%d protocol=%d sport=%hu dport=%hu saddr=%pI4 daddr=%pI4 saddrv6=%pI6c daddrv6=%pI6 cpu=%d hash=%u",
		__entry->family,
		__entry->protocol,
		__entry->sport, __entry->dport,
		__entry->saddr, __entry->daddr,
		__entry->saddr_v6, __entry->daddr_v6,
		__entry->cpu, __entry->hash)
);

TRACE_EVENT(sk_rps_flow_update,
	TP_PROTO(int flow_id, int filter_id, int rxq, int next_cpu),
	TP_ARGS(flow_id, filter_id, rxq, next_cpu),
	TP_STRUCT__entry(
		__field(int, flow_id)
		__field(int, filter_id)
		__field(int, rxq)
		__field(int, next_cpu)
	),
	TP_fast_assign(
		__entry->flow_id = flow_id;
		__entry->filter_id = filter_id;
		__entry->rxq = rxq;
		__entry->next_cpu = next_cpu;
	),
	TP_printk("flow_id=%d filter_id=%d rxq=%d next_cpu=%d",
		__entry->flow_id, __entry->filter_id, __entry->rxq, __entry->next_cpu)
);

#endif /* _TRACE_RPS_H */

/* This part must be outside protection */
#include <trace/define_trace.h>