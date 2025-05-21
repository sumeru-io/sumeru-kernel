/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_CACHEFLOW_H
#define __CACHEFLOW_CACHEFLOW_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/skbuff.h>
#include <linux/jump_label.h>
#include <net/page_pool/helpers.h>
#include <trace/events/skb.h>

extern u8 cacheflow_mark_enable;
extern u8 cacheflow_track_enable;

extern struct static_key_false cacheflow_steer_enable;

extern int cacheflow_steer_core;
extern int cacheflow_buffer_size;
extern int cacheflow_thresh;
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

static inline bool is_cacheflow_track_enabled(void)
{
	return READ_ONCE(cacheflow_track_enable) > 0;
}

static inline bool is_cacheflow_mark_enabled(void)
{
	return READ_ONCE(cacheflow_mark_enable) > 0;
}

static inline bool is_cacheflow_steer_enabled(void)
{
	return static_branch_likely(&cacheflow_steer_enable);
}

static inline int get_cacheflow_pool_size(void) {
	return READ_ONCE(cacheflow_buffer_size);
}

static inline int get_cacheflow_thresh(void) {
	return READ_ONCE(cacheflow_thresh);
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
	if (CACHEFLOW_GET_PFLAG(skb, SKB_CACHEFLOW) && skb->head) {
		trace_skb_cacheflow_memory_location(page_to_netmem(virt_to_page(skb->head)), location);

		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			trace_skb_cacheflow_memory_location(page_to_netmem(netmem_to_page(skb_shinfo(skb)->frags[i].netmem)), location);
		}
	}
}

#endif /* __CACHEFLOW_CACHEFLOW_H */
