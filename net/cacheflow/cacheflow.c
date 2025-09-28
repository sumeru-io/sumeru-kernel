// SPDX-License-Identifier: GPL-2.0
/*
 * cacheflow.c
 *	Author:	Minhu Wang <minhuw@acm.org>
 */

#include <linux/cache.h>
#include <linux/jump_label.h>
#include <linux/tcp.h>
#include <linux/sock_diag.h>
#include <asm/msr.h>

#include <net/cacheflow/cacheflow.h>
#include <net/cacheflow/page_pool.h>
#include <net/sock.h>

#include <trace/events/cacheflow.h>

/* Intel CAT MSR definitions */
#define IA32_L3_QOS_MASK_BASE   0xC90    /* Base MSR for L3 CBM */

struct static_key_false cacheflow_steer_enable __read_mostly;
EXPORT_SYMBOL(cacheflow_steer_enable);

atomic_t cacheflow_id_counter = ATOMIC_INIT(0);

int cacheflow_thread __read_mostly;
EXPORT_SYMBOL(cacheflow_thread);
int cacheflow_steer_core __read_mostly;
EXPORT_SYMBOL(cacheflow_steer_core);
int cacheflow_stack_cores[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(cacheflow_stack_cores);
int cacheflow_stack_cores_num __read_mostly;
EXPORT_SYMBOL(cacheflow_stack_cores_num);

int cacheflow_aqm __read_mostly;
EXPORT_SYMBOL(cacheflow_aqm);
int cacheflow_target __read_mostly = 2048;
EXPORT_SYMBOL(cacheflow_target);
int cacheflow_thresh __read_mostly = 65536;
EXPORT_SYMBOL(cacheflow_thresh);

int cacheflow_alpha __read_mostly = 2;
EXPORT_SYMBOL(cacheflow_alpha);
int cacheflow_beta __read_mostly = 1;
EXPORT_SYMBOL(cacheflow_beta);

int cacheflow_schedule __read_mostly;
EXPORT_SYMBOL(cacheflow_schedule);

int cacheflow_ack_mod __read_mostly;
EXPORT_SYMBOL(cacheflow_ack_mod);

int cacheflow_ipi_packet_thresh __read_mostly = 16;
EXPORT_SYMBOL(cacheflow_ipi_packet_thresh);
int cacheflow_ipi_usec_thresh __read_mostly = 64;
EXPORT_SYMBOL(cacheflow_ipi_usec_thresh);
int cacheflow_elephant_flow_thresh __read_mostly = 256;

int cacheflow_pp_anneal_size __read_mostly = 256;
EXPORT_SYMBOL(cacheflow_pp_anneal_size);

int cacheflow_napi_weight __read_mostly = 16;
EXPORT_SYMBOL(cacheflow_napi_weight);

int cacheflow_pool_size __read_mostly = 4096;
EXPORT_SYMBOL(cacheflow_pool_size);

int cacheflow_cache_boost __read_mostly = 0;
EXPORT_SYMBOL(cacheflow_cache_boost);
int cacheflow_cache_cos __read_mostly = 0;
EXPORT_SYMBOL(cacheflow_cache_cos);
int cacheflow_cache_min_ways __read_mostly = 2;
EXPORT_SYMBOL(cacheflow_cache_min_ways);
int cacheflow_cache_max_ways __read_mostly = 8;
EXPORT_SYMBOL(cacheflow_cache_max_ways);

int cacheflow_buffer_quantum __read_mostly = 4096;
EXPORT_SYMBOL(cacheflow_buffer_quantum);

int cacheflow_cache_boost_interval_us __read_mostly = 100;
EXPORT_SYMBOL(cacheflow_cache_boost_interval_us);
int cacheflow_cache_expand_left __read_mostly = 0;
EXPORT_SYMBOL(cacheflow_cache_expand_left);
int cacheflow_cache_boost_factor __read_mostly = 32;
EXPORT_SYMBOL(cacheflow_cache_boost_factor);

/* Cache way size in KB, initialized once during boot */
static u32 cacheflow_cache_way_size_kb __read_mostly = 0;

int cacheflow_should_mark(struct cacheflow_page_pool *pool, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 sock_recv_len = tp->rcv_nxt - tp->copied_seq;
	u32 sock_backlog_len = sk->sk_backlog.len;
	u32 sock_qlen = sock_recv_len + sock_backlog_len;
	u32 rtt = tp->rcv_rate_est.delta >> cacheflow_alpha;
	u32 drain_rate = tp->rcv_rate_est.copied_rate >> cacheflow_alpha;
	u32 thresh = READ_ONCE(cacheflow_thresh);
	u32 target = READ_ONCE(cacheflow_target);
	u32 allocated_pages = READ_ONCE(pool->allocated_pages);
	u32 free_pages = READ_ONCE(pool->cache_pages);
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
			mark = ((u64)sock_qlen * rtt * 12500) >
				((u64)remaining_pages * tcp_sk(sk)->mss_cache * drain_rate * cacheflow_beta);
		}
		break;
	case 3:
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

/**
 * cacheflow_init_cache_way_size - Initialize L3 cache way size
 *
 * Called once during boot to calculate and cache the size of one L3 cache way
 * using CPUID to detect the total number of cache ways, similar to resctrl.
 * On failure, disables cache boost by setting way size to 0.
 */
static void __init cacheflow_init_cache_way_size(void)
{
	u32 total_cache_kb = boot_cpu_data.x86_cache_size;
	u32 eax, ebx, ecx, edx;
	u32 cbm_len;

	if (!total_cache_kb) {
		pr_err("cacheflow: cache size detection failed - cache boost disabled\n");
		cacheflow_cache_way_size_kb = 0; /* Disable cache boost */
		return;
	}

	/* Use CPUID 0x10,1 to get L3 cache allocation info
	 * Similar to what resctrl does in rdt_get_cache_alloc_cfg()
	 */
	if (boot_cpu_data.cpuid_level < 0x10) {
		pr_err("cacheflow: CPUID level 0x10 not supported - cache boost disabled\n");
		cacheflow_cache_way_size_kb = 0; /* Disable cache boost */
		return;
	}

	cpuid_count(0x00000010, 1, &eax, &ebx, &ecx, &edx);
	cbm_len = (eax & 0x1f) + 1; /* Bits 4:0 contain CBM length - 1 */

	if (!cbm_len) {
		pr_err("cacheflow: invalid CBM length detected - cache boost disabled\n");
		cacheflow_cache_way_size_kb = 0; /* Disable cache boost */
		return;
	}

	cacheflow_cache_way_size_kb = total_cache_kb / cbm_len;
	pr_info("cacheflow: cache way size detected: %u KB (%u total KB, %u ways)\n",
		cacheflow_cache_way_size_kb, total_cache_kb, cbm_len);
}

int cacheflow_should_boost(struct cacheflow_page_pool *pool)
{
	u32 cos_id = READ_ONCE(cacheflow_cache_cos);
	u32 allocated_pages = READ_ONCE(pool->allocated_pages);
	u32 current_ways = 0, max_ways = 0, min_ways = 0;
	u32 way_size_kb = cacheflow_cache_way_size_kb;
	u32 current_cache_kb = 0;
	u32 quantum = 0;
	u64 estimated_usage_kb = 0;
	u32 boost_factor = READ_ONCE(cacheflow_cache_boost_factor);
	int decision;

	/* Cache boost disabled */
	if (!READ_ONCE(cacheflow_cache_boost) || !cos_id || !way_size_kb)
		return CACHEFLOW_CACHE_KEEP;

	current_ways = cacheflow_cache_get_current_ways();
	max_ways = READ_ONCE(cacheflow_cache_max_ways);
	min_ways = READ_ONCE(cacheflow_cache_min_ways);

	quantum = READ_ONCE(cacheflow_buffer_quantum);
	estimated_usage_kb = ((u64)allocated_pages * quantum) >> 10;
	current_cache_kb = current_ways * way_size_kb;

	/* Check if we should boost */
	if (current_ways < max_ways) {
		if (estimated_usage_kb > ((u64)current_cache_kb * boost_factor >> 6)) {
			decision = CACHEFLOW_CACHE_BOOST;
			goto trace_and_return;
		}
	}

	/* Check if we should shrink */
	if (current_ways > min_ways) {
		u32 shrink_cache_kb = (current_ways - 1) * way_size_kb;
		if (estimated_usage_kb < ((u64)shrink_cache_kb * boost_factor >> 6)) {
			decision = CACHEFLOW_CACHE_SHRINK;
			goto trace_and_return;
		}
	}

	decision = CACHEFLOW_CACHE_KEEP;

trace_and_return:
	trace_cacheflow_cache_boost_decision(pool, allocated_pages, current_ways,
					     max_ways, min_ways, (u32)estimated_usage_kb,
					     current_cache_kb, decision);
	return decision;
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

static int __init cacheflow_init(void)
{
	cacheflow_init_cache_way_size();
	return 0;
}

core_initcall(cacheflow_init);
