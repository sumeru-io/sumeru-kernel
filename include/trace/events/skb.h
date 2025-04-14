/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM skb

#if !defined(_TRACE_SKB_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SKB_H

#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/tracepoint.h>

#undef FN
#define FN(reason)	TRACE_DEFINE_ENUM(SKB_DROP_REASON_##reason);
DEFINE_DROP_REASON(FN, FN)

#undef FN
#undef FNe
#define FN(reason)	{ SKB_DROP_REASON_##reason, #reason },
#define FNe(reason)	{ SKB_DROP_REASON_##reason, #reason }

/*
 * Tracepoint for free an sk_buff:
 */
TRACE_EVENT(kfree_skb,

	TP_PROTO(struct sk_buff *skb, void *location,
		 enum skb_drop_reason reason, struct sock *rx_sk),

	TP_ARGS(skb, location, reason, rx_sk),

	TP_STRUCT__entry(
		__field(void *,		skbaddr)
		__field(void *,		location)
		__field(void *,		rx_sk)
		__field(unsigned short,	protocol)
		__field(enum skb_drop_reason,	reason)
	),

	TP_fast_assign(
		__entry->skbaddr = skb;
		__entry->location = location;
		__entry->rx_sk = rx_sk;
		__entry->protocol = ntohs(skb->protocol);
		__entry->reason = reason;
	),

	TP_printk("skbaddr=%p rx_sk=%p protocol=%u location=%pS reason: %s",
		  __entry->skbaddr, __entry->rx_sk, __entry->protocol,
		  __entry->location,
		  __print_symbolic(__entry->reason,
				   DEFINE_DROP_REASON(FN, FNe)))
);

#undef FN
#undef FNe

TRACE_EVENT(consume_skb,

	TP_PROTO(struct sk_buff *skb, void *location),

	TP_ARGS(skb, location),

	TP_STRUCT__entry(
		__field(	void *,	skbaddr)
		__field(	void *,	location)
	),

	TP_fast_assign(
		__entry->skbaddr = skb;
		__entry->location = location;
	),

	TP_printk("skbaddr=%p location=%pS", __entry->skbaddr, __entry->location)
);

TRACE_EVENT(skb_copy_datagram_iovec,

	TP_PROTO(const struct sk_buff *skb, int len),

	TP_ARGS(skb, len),

	TP_STRUCT__entry(
		__field(	const void *,		skbaddr		)
		__field(	int,			len		)
	),

	TP_fast_assign(
		__entry->skbaddr = skb;
		__entry->len = len;
	),

	TP_printk("skbaddr=%p len=%d", __entry->skbaddr, __entry->len)
);

TRACE_EVENT(skb_ring_timestamp,
	TP_PROTO(const struct sk_buff *skb, u32 queue_index, u64 receive_timestamp, u64 process_timestamp),
	TP_ARGS(skb, queue_index, receive_timestamp, process_timestamp),
	TP_STRUCT__entry(
		__field(const void *, skb)
		__field(u32, queue_index)
		__field(u64, receive_timestamp)
		__field(u64, process_timestamp)
	),
	TP_fast_assign(
		__entry->skb = skb;
		__entry->queue_index = queue_index;
		__entry->receive_timestamp = receive_timestamp;
		__entry->process_timestamp = process_timestamp;
	),
	TP_printk("skbaddr=%p queue=%u recv=%llu proc=%llu",
		  __entry->skb,
		  __entry->queue_index,
		  __entry->receive_timestamp,
		  __entry->process_timestamp)
);

TRACE_EVENT(skb_sock_timestamp,
	TP_PROTO(const struct sk_buff *skb, u32 skb_len, u64 sock_id, u64 enqueue_timestamp, u64 consume_timestamp),
	TP_ARGS(skb, skb_len, sock_id, enqueue_timestamp, consume_timestamp),
	TP_STRUCT__entry(
		__field(const void *, skb)
		__field(u32, skb_len)
		__field(u64, sock_id)
		__field(u64, enqueue_timestamp)
		__field(u64, consume_timestamp)
	),
	TP_fast_assign(
		__entry->skb = skb;
		__entry->skb_len = skb_len;
		__entry->sock_id = sock_id;
		__entry->enqueue_timestamp = enqueue_timestamp;
		__entry->consume_timestamp = consume_timestamp;
	),
	TP_printk("skbaddr=%p skb_len=%lu sock_id=%llu enq=%llu cons=%llu",
		  __entry->skb,
		  __entry->skb_len,
		  __entry->sock_id,
		  __entry->enqueue_timestamp,
		  __entry->consume_timestamp)
);



#endif /* _TRACE_SKB_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
