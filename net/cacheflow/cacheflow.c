// SPDX-License-Identifier: GPL-2.0
/*
 * cacheflow.c
 *	Author:	Minhu Wang <minhuw@acm.org>
 */

#include <linux/cache.h>
#include <linux/jump_label.h>
#include <linux/tcp.h>
#include <linux/sock_diag.h>

#include <net/cacheflow/cacheflow.h>
#include <net/cacheflow/page_pool.h>
#include <net/sock.h>

#include <trace/events/cacheflow.h>

struct static_key_false cacheflow_steer_enable __read_mostly;
EXPORT_SYMBOL(cacheflow_steer_enable);

atomic_t cacheflow_id_counter = ATOMIC_INIT(0);

int cacheflow_thread __read_mostly;
int cacheflow_steer_core __read_mostly;
int cacheflow_stack_cores[NR_CPUS] __read_mostly;
int cacheflow_stack_cores_num __read_mostly;

int cacheflow_aqm __read_mostly;
int cacheflow_target __read_mostly = 2048;
int cacheflow_thresh __read_mostly = 65536;

int cacheflow_alpha __read_mostly = 2;
int cacheflow_beta __read_mostly = 1;

int cacheflow_schedule __read_mostly;

int cacheflow_ack_mod __read_mostly;

int cacheflow_ipi_packet_thresh __read_mostly = 16;
int cacheflow_ipi_usec_thresh __read_mostly = 64;
int cacheflow_elephant_flow_thresh __read_mostly = 256;

int cacheflow_should_mark(struct cacheflow_page_pool *pool, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 sock_recv_len = tp->rcv_nxt - tp->copied_seq;
	u32 sock_backlog_len = sk->sk_backlog.len;
	u32 sock_qlen = sock_recv_len + sock_backlog_len;
	u32 rtt = tp->rcv_rate_est.delta >> cacheflow_alpha;
	u32 drain_rate = tp->rcv_rate_est.copied_rate >> cacheflow_alpha;
	u32 recv_rate = tp->rcv_rate_est.recv_rate >> cacheflow_alpha;
	u32 thresh = READ_ONCE(cacheflow_thresh);
	u32 target = READ_ONCE(cacheflow_target);
	u32 allocated_pages = READ_ONCE(pool->allocated_pages);
	u32 free_pages = READ_ONCE(pool->ring_pages) + READ_ONCE(pool->array_pages);
	u32 remaining_pages =
		target > allocated_pages ? target - allocated_pages : 0;
	int mark = 0;

	switch (READ_ONCE(cacheflow_aqm)) {
	case 0:
		break;
	case 1:
		mark = (allocated_pages >= thresh);
		break;
	case 2:
		if (sock_qlen > thresh) {
			mark = ((u64)sock_qlen * rtt * 3) >
			       ((u64)remaining_pages * drain_rate * cacheflow_beta);
		}
		break;
	case 3:
		if (sock_qlen > thresh) {
			mark = ((u64)sock_qlen * rtt * 3) * (u64)U32_MAX >
			       (((u64)remaining_pages *
				 drain_rate * cacheflow_beta) *
				(u64)get_random_u32());
		}
		break;
	case 4:
		if (sock_qlen > thresh) {
			mark = ((u64)sock_qlen * rtt * 12500) >
				((u64)remaining_pages * tcp_sk(sk)->mss_cache * drain_rate * cacheflow_beta);
		}
		break;
	case 5:
		if (sock_qlen > thresh) {
			mark = ((u64)sock_qlen * rtt * 12500) >
				((((u64)remaining_pages * tcp_sk(sk)->mss_cache * drain_rate * cacheflow_beta) >> 24) * (u64)get_random_u32()) >> 8;
		}
		break;
	default:
		pr_err("cacheflow: unknown AQM mode: %d\n",
		       READ_ONCE(cacheflow_aqm));
		mark = 0;
	}

	trace_cacheflow_mark(__sock_gen_cookie(sk), tp->cacheflow_id, allocated_pages, free_pages, thresh,
			     sock_recv_len, sock_backlog_len, rtt, tp, mark);

	return mark;
}

int cacheflow_should_ack(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	if (!tp->cacheflow_pool)
		return 1;

	return !cacheflow_should_mark(tp->cacheflow_pool, sk);
}

int cacheflow_schedule_priority(struct cacheflow_page_pool *pool, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;
	u32 sock_recv_len = tp->rcv_nxt - tp->copied_seq;
	u32 sock_backlog_len = sk->sk_backlog.len;
	int qlen = sock_recv_len + sock_backlog_len;
	int priority = 0;

	switch (READ_ONCE(cacheflow_schedule)) {
	case 0:
		return 0;
	case 1:
		priority = 0 - min((qlen) >> 16, 20);
		break;
	case 2:
		priority = 0 - (min(ilog2((qlen >> 17) + 1), 20));
		break;
	case 3:
		if ((skb = skb_peek(&sk->sk_receive_queue))) {
			if (ktime_get_real_ns() - skb_shinfo(skb)->ms_timestamp.enqueue_timestamp > 1000000) {
				priority = -5;
			}
		}
		break;
	default:
		return 0;
	}

	if (priority != tp->drain_priority) {
		set_user_nice(tp->drain_task, priority);
		tcp_sk(sk)->drain_priority = priority;
		trace_cacheflow_schedule_priority(__sock_gen_cookie(sk), tp->cacheflow_id, sock_recv_len, sock_backlog_len, tp->drain_task->pid, priority);
	}

	return 0;
}
