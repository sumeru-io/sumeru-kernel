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

u8 cacheflow_mark_enable __read_mostly;

struct static_key_false cacheflow_steer_enable __read_mostly;
EXPORT_SYMBOL(cacheflow_steer_enable);

int cacheflow_steer_core __read_mostly;
int cacheflow_buffer_size __read_mostly = 4096;
int cacheflow_thresh __read_mostly = 2048;
int cacheflow_aqm __read_mostly;
int cacheflow_ipi_packet_thresh __read_mostly = 16;
int cacheflow_ipi_usec_thresh __read_mostly = 128;
int cacheflow_elephant_flow_thresh __read_mostly = 256;

static int cacheflow_should_mark_direct(struct cacheflow_page_pool *pool, struct sock *sk)
{
	int mark = READ_ONCE(pool->allocated_pages) >= READ_ONCE(cacheflow_thresh);

	trace_cacheflow_direct_mark(__sock_gen_cookie(sk), pool->allocated_pages, READ_ONCE(cacheflow_thresh), mark);

	return mark;
}

// Based on the paper "ABM: Active Buffer Management in Datacenters [SIGCOMM '22]"
static int cacheflow_should_mark_abm(struct cacheflow_page_pool *pool, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 sock_qlen = tp->rcv_nxt - tp->copied_seq + sk->sk_backlog.len;
	u32 rtt = tp->rcv_rtt_est.rtt_us >> 3;
	u32 drain_rate = tp->rcv_rate_est.copied_rate >> 3;
	u32 thresh = READ_ONCE(cacheflow_thresh);
	u32 allocated_pages = READ_ONCE(pool->allocated_pages);

	int mark = sock_qlen * rtt * 12500 > (thresh - allocated_pages) * PAGE_SIZE * drain_rate;

	trace_cacheflow_abm_mark(__sock_gen_cookie(sk), allocated_pages, thresh, sock_qlen, rtt, drain_rate, mark);

	return mark;
}

int cacheflow_should_mark(struct cacheflow_page_pool *pool, struct sock *sk)
{
	switch (READ_ONCE(cacheflow_aqm)) {
	case 0:
		return cacheflow_should_mark_direct(pool, sk);
	case 1:
		return cacheflow_should_mark_abm(pool, sk);
	default:
		return 0;
	}
}

