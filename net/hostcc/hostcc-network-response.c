// SPDX-License-Identifier: GPL-2.0
/*
 * HostCC network response implementation
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

#include "hostcc.h"
#include "hostcc-signals.h"
#include "hostcc-local-response.h"
#include "hostcc-network-response.h"
#include "hostcc-logging.h"
#include "hostcc-sysfs.h"

DEFINE_SPINLOCK(etx_spinlock_rx);
DEFINE_SPINLOCK(etx_spinlock_tx);

/* Global netfilter hook pointers */
struct nf_hook_ops *nf_markecn_ops_rx = NULL;
struct nf_hook_ops *nf_markecn_ops_tx = NULL;

/* Netfilter logic to mark ECN bits */
void sample_counters_nf(int c)
{
	if (hostcc_mode == HOSTCC_MODE_RX) {
		latest_measured_avg_occ_wr_nf = smoothed_avg_occ_wr >> 10;
	} else {
		latest_measured_avg_occ_rd_nf = smoothed_avg_occ_rd >> 10;
	}

	tsc_sample_nf = hostcc_read_tsc();
	prev_rdtsc_nf = cur_rdtsc_nf;
	cur_rdtsc_nf = tsc_sample_nf;

	/* Convert TSC cycles to nanoseconds using kernel's TSC frequency */
	extern unsigned int tsc_khz;
	latest_time_delta_nf_ns = ((cur_rdtsc_nf - prev_rdtsc_nf) * 1000000ULL) / tsc_khz;
}

unsigned int nf_markecn_handler_rx(void *priv, struct sk_buff *skb,
				   const struct nf_hook_state *state)
{
	struct net_device *indev;
	const char *interfaceName;
	int cpu;

	/* Check for valid skb */
	if (!skb)
		return NF_ACCEPT;

	/* Get input device */
	indev = state->in;

	if (!indev)
		return NF_ACCEPT;

	interfaceName = indev->name;
	if (strcmp(interfaceName, hostcc_nic_interface) != 0)
		return NF_ACCEPT;

	/* Process RX packet */
	{
		struct iphdr *iph = ip_hdr(skb);

		if (!iph)
			return NF_ACCEPT;

		spin_lock(&etx_spinlock_rx);
		latest_datagram_len = ntohs(iph->tot_len);
		cpu = get_cpu();
		sample_counters_nf(cpu);
		put_cpu();

		if (hostcc_enable_logging && hostcc_ecn_logging)
			update_log_nf(cpu);

		if (hostcc_enable_network_response) {
			if (latest_measured_avg_occ_wr_nf >
			    hostcc_target_iio_wr_thresh) {
				/* Mark ECN CE (Congestion Experienced) */
				iph->tos |= INET_ECN_CE;
				/* Recalculate checksum */
				iph->check = 0;
				ip_send_check(iph);
			}
		}
		spin_unlock(&etx_spinlock_rx);
	}

	return NF_ACCEPT;
}

unsigned int nf_markecn_handler_tx(void *priv, struct sk_buff *skb,
				   const struct nf_hook_state *state)
{
	struct net_device *outdev;
	const char *interfaceName;
	int cpu;

	/* Check for valid skb */
	if (!skb)
		return NF_ACCEPT;

	/* Get output device */
	outdev = state->out;

	if (!outdev)
		return NF_ACCEPT;

	interfaceName = outdev->name;
	if (strcmp(interfaceName, hostcc_nic_interface) != 0)
		return NF_ACCEPT;

	/* Process TX packet */
	{
		struct iphdr *iph = ip_hdr(skb);

		if (!iph)
			return NF_ACCEPT;

		spin_lock(&etx_spinlock_tx);
		latest_datagram_len = ntohs(iph->tot_len);
		cpu = get_cpu();
		sample_counters_nf(cpu);
		put_cpu();

		if (hostcc_enable_logging && hostcc_ecn_logging)
			update_log_nf(cpu);

		if (hostcc_enable_network_response) {
			if (latest_measured_avg_occ_rd_nf >
			    hostcc_target_iio_rd_thresh) {
				/* Mark ECN CE (Congestion Experienced) */
				iph->tos |= INET_ECN_CE;
				/* Recalculate checksum */
				iph->check = 0;
				ip_send_check(iph);
			}
		}
		spin_unlock(&etx_spinlock_tx);
	}

	return NF_ACCEPT;
}

int nf_init(void)
{
	int ret = 0;

	if (hostcc_mode == HOSTCC_MODE_RX) {
		/* Pre-routing hook for Rx datapath */
		nf_markecn_ops_rx =
			kcalloc(1, sizeof(struct nf_hook_ops), GFP_KERNEL);
		if (!nf_markecn_ops_rx)
			return -ENOMEM;

		nf_markecn_ops_rx->hook = nf_markecn_handler_rx;
		nf_markecn_ops_rx->hooknum = NF_INET_PRE_ROUTING;
		nf_markecn_ops_rx->pf = NFPROTO_IPV4;
		nf_markecn_ops_rx->priority = NF_IP_PRI_FIRST + 1;

		ret = nf_register_net_hook(&init_net, nf_markecn_ops_rx);
		if (ret) {
			kfree(nf_markecn_ops_rx);
			nf_markecn_ops_rx = NULL;
			return ret;
		}
	} else {
		/* Post-routing hook for Tx datapath */
		nf_markecn_ops_tx =
			kcalloc(1, sizeof(struct nf_hook_ops), GFP_KERNEL);
		if (!nf_markecn_ops_tx)
			return -ENOMEM;

		nf_markecn_ops_tx->hook = nf_markecn_handler_tx;
		nf_markecn_ops_tx->hooknum = NF_INET_POST_ROUTING;
		nf_markecn_ops_tx->pf = NFPROTO_IPV4;
		nf_markecn_ops_tx->priority = NF_IP_PRI_FIRST + 1;

		ret = nf_register_net_hook(&init_net, nf_markecn_ops_tx);
		if (ret) {
			kfree(nf_markecn_ops_tx);
			nf_markecn_ops_tx = NULL;
			return ret;
		}
	}

	init_nf_log();
	pr_info("HostCC: Netfilter hooks initialized\n");
	return 0;
}

void nf_exit(void)
{
	if (hostcc_mode == HOSTCC_MODE_RX) {
		if (nf_markecn_ops_rx) {
			nf_unregister_net_hook(&init_net, nf_markecn_ops_rx);
			kfree(nf_markecn_ops_rx);
			nf_markecn_ops_rx = NULL;
		}
	} else {
		if (nf_markecn_ops_tx) {
			nf_unregister_net_hook(&init_net, nf_markecn_ops_tx);
			kfree(nf_markecn_ops_tx);
			nf_markecn_ops_tx = NULL;
		}
	}

	pr_info("HostCC: Ending ECN Marking\n");
	if (hostcc_ecn_logging)
		dump_nf_log();
}