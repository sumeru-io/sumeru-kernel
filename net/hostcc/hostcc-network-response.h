/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HostCC network response header
 *
 * Based on HostCC implementation from original research paper
 * Original author: Saksham Agarwal
 * Original code: https://github.com/Terabit-Ethernet/hostCC
 * 
 * Note: Original code had no explicit license
 * Used as research baseline under academic fair use
 *
 * Modified for built-in kernel integration
 * Copyright (C) 2025 Minhu Wang, Tsinghua University
 */

#ifndef HOSTCC_NETWORK_RESPONSE_H
#define HOSTCC_NETWORK_RESPONSE_H

#include <linux/skbuff.h>
#include <linux/netfilter.h>

/* Global netfilter hook pointers */
extern struct nf_hook_ops *nf_markecn_ops_rx;
extern struct nf_hook_ops *nf_markecn_ops_tx;

enum {
	INET_ECN_NOT_ECT = 0,
	INET_ECN_ECT_1 = 1,
	INET_ECN_ECT_0 = 2,
	INET_ECN_CE = 3,
	INET_ECN_MASK = 3,
};

void sample_counters_nf(int c);
unsigned int nf_markecn_handler_rx(void *priv, struct sk_buff *skb,
				   const struct nf_hook_state *state);
unsigned int nf_markecn_handler_tx(void *priv, struct sk_buff *skb,
				   const struct nf_hook_state *state);
int nf_init(void);
void nf_exit(void);

#endif