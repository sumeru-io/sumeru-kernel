/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_CACHEFLOW_H
#define __CACHEFLOW_CACHEFLOW_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/skbuff.h>
#include <linux/jump_label.h>
#include <net/page_pool/helpers.h>
#include <trace/events/skb.h>

extern struct static_key_false cacheflow_steer_enable;
extern struct static_key_false cacheflow_steer_page_clear;
extern atomic_t cacheflow_id_counter;

extern int cacheflow_steer_core;
extern int cacheflow_thread;
extern int cacheflow_stack_cores[NR_CPUS];
extern int cacheflow_stack_cores_num;

extern int cacheflow_aqm;
extern int cacheflow_alpha;
extern int cacheflow_beta;

extern int cacheflow_thresh;
extern int cacheflow_target;

extern int cacheflow_schedule;

extern int cacheflow_ack_mod;

extern int cacheflow_elephant_flow_thresh;

extern int cacheflow_ipi_packet_thresh;
extern int cacheflow_ipi_usec_thresh;

enum {
	NETMEM_LOCATION_POOL = 0,
	NETMEM_LOCATION_RING = 1,
	NETMEM_LOCATION_NAPI = 2,
	NETMEM_LOCATION_STACK = 3,
	NETMEM_LOCATION_SOCKET = 4,
	NETMEM_LOCATION_RECYCLE = 5,
};


enum sk_cacheflow_flag {
	SK_CACHEFLOW_ELEPHANT_FLOW,
	SK_CACHEFLOW_ACK_MODERATE,
	SK_CACHEFLOW_UNSCHED_FLOW,
	SK_CACHEFLOW_NUM_FLAGS
};

#define CACHEFLOW_SK_SET_FLAG(tp, pflag, enable)			\
	do {								\
		if (enable)						\
			(tp)->cacheflow |= BIT(pflag);			\
		else							\
			(tp)->cacheflow &= ~(BIT(pflag));		\
	} while (0)

#define CACHEFLOW_SK_GET_FLAG(tp, pflag) (!!((tp)->cacheflow & (BIT(pflag))))

int cacheflow_should_mark(struct cacheflow_page_pool *pool, struct sock *sk);
int cacheflow_should_ack(struct sock *sk);
int cacheflow_schedule_priority(struct cacheflow_page_pool *pool, struct sock *sk);

static inline bool is_cacheflow_steer_enabled(void)
{
	return static_branch_likely(&cacheflow_steer_enable);
}

static inline bool is_cacheflow_steer_page_clear_enabled(void)
{
	return static_branch_likely(&cacheflow_steer_page_clear);
}

static inline int get_cacheflow_steer_core(void) {
	return READ_ONCE(cacheflow_steer_core);
}

static inline int get_cacheflow_ipi_packet_thresh(void) {
	return READ_ONCE(cacheflow_ipi_packet_thresh);
}

static inline int get_cacheflow_ipi_usec_thresh(void) {
	return READ_ONCE(cacheflow_ipi_usec_thresh);
}

static inline int get_cacheflow_elephant_flow_thresh(void) {
	return READ_ONCE(cacheflow_elephant_flow_thresh);
}

static inline void cacheflow_track_page_move(struct sk_buff *skb, int location)
{
	int i;
	if (CACHEFLOW_GET_FLAG(skb, SKB_CACHEFLOW) && skb->head) {
		trace_skb_cacheflow_memory_location(page_to_netmem(virt_to_page(skb->head)), location);

		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			trace_skb_cacheflow_memory_location(page_to_netmem(netmem_to_page(skb_shinfo(skb)->frags[i].netmem)), location);
		}
	}
}

static inline const char *cacheflow_aqm_to_str(int aqm)
{
	switch (aqm) {
	case 0:
		return "off";
	case 1:
		return "direct";
	case 2:
		return "abm";
	default:
		return "unknown";
	}
}

static inline int show_cacheflow_stack_cores(char *buf, int size)
{
	int i, len = 0;

	if (size < 1024)
		return 0;

	len += snprintf(buf + len, sizeof(buf) - len,
		 "cacheflow: stack cores num: %d, [ ", cacheflow_stack_cores_num);

	for (i = 0; i < cacheflow_stack_cores_num; i++)
		len += snprintf(buf + len, sizeof(buf) - len,
			i == 0 ? "%d" : ", %d", cacheflow_stack_cores[i]);

	len += snprintf(buf + len, sizeof(buf) - len, " ]\n");

	return len;
}

#endif /* __CACHEFLOW_CACHEFLOW_H */
