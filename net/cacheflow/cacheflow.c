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

int cacheflow_steer_core __read_mostly;
int cacheflow_thresh __read_mostly = 2048;
int cacheflow_aqm __read_mostly;
int cacheflow_alpha __read_mostly = 2;
int cacheflow_ipi_packet_thresh __read_mostly = 16;
int cacheflow_ipi_usec_thresh __read_mostly = 128;
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
	u32 allocated_pages = READ_ONCE(pool->allocated_pages);
	u32 remaining_pages =
		thresh > allocated_pages ? thresh - allocated_pages : 0;
	int mark = 0;

	switch (READ_ONCE(cacheflow_aqm)) {
	case 0:
		break;
	case 1:
		mark = (allocated_pages >= thresh);
		break;
	case 2:
		if (sock_qlen > 131072) {
			// Based on the paper "ABM: Active Buffer Management in Datacenters [SIGCOMM '22]"
			mark = ((u64)sock_qlen * rtt * 3) >
			       ((u64)remaining_pages * drain_rate);
		}
		break;
	case 3:
		if (sock_qlen > 131072) {
			mark = ((u64)sock_qlen * rtt * 3) * (2 * (u64)U32_MAX) >
			       (((u64)remaining_pages *
				 drain_rate) *
				(((u64)get_random_u32() + U32_MAX)));
		}
		break;
	default:
		pr_err("cacheflow: unknown AQM mode: %d\n",
		       READ_ONCE(cacheflow_aqm));
		mark = 0;
	}

	trace_cacheflow_mark(__sock_gen_cookie(sk), allocated_pages, thresh,
			     sock_recv_len, sock_backlog_len, rtt, drain_rate,
			     recv_rate, mark);

	return mark;
}
