/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HostCC sysfs interface header
 *
 * Based on HostCC implementation from original research paper
 * Original author: Saksham Agarwal
 * Original code: https://github.com/Terabit-Ethernet/hostCC
 * 
 * Note: Original code had no explicit license
 * Used as research baseline under academic fair use
 *
 * Sysfs interface implementation for built-in kernel integration
 * Copyright (C) 2025 Minhu Wang, Tsinghua University
 */

#ifndef HOSTCC_SYSFS_H
#define HOSTCC_SYSFS_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/threads.h>
#include <linux/delay.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/string.h>
#include <net/ip.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <linux/random.h>
#include <linux/workqueue.h>

/* Global HostCC parameters accessible through sysfs */
extern int hostcc_mem_contender_pid;
extern int hostcc_target_pcie_thresh;
extern int hostcc_target_iio_wr_thresh;
extern int hostcc_target_iio_rd_thresh;
extern int hostcc_enable_network_response;
extern int hostcc_enable_local_response;
extern int hostcc_mode;
extern char hostcc_nic_interface[16];
extern int hostcc_nic_local_socket;
extern int hostcc_nic_iio_stack;
/* MBA core list configuration - separate arrays for each throttling level */
#define HOSTCC_MAX_MBA_CORES 16
extern int hostcc_mba_level_1_cores[HOSTCC_MAX_MBA_CORES];
extern int hostcc_mba_level_1_core_count;
extern int hostcc_mba_level_2_cores[HOSTCC_MAX_MBA_CORES];
extern int hostcc_mba_level_2_core_count;
extern int hostcc_mba_level_3_cores[HOSTCC_MAX_MBA_CORES];
extern int hostcc_mba_level_3_core_count;
extern int hostcc_mba_val_high;
extern int hostcc_mba_val_low;
extern int hostcc_mba_cos_id;
extern int hostcc_use_process_scheduler;
extern int hostcc_iio_core;
extern int hostcc_iio_logging;
extern int hostcc_pcie_core;
extern int hostcc_pcie_logging;
extern int hostcc_ecn_logging;
extern int hostcc_log_size;

/* Runtime control parameters */
extern int hostcc_enable;
extern int hostcc_enable_logging;

/* Function prototypes */
int hostcc_sysfs_init(void);
void hostcc_sysfs_cleanup(void);

/* Core enable/disable functionality */
int hostcc_set_enable(int enable);

#endif /* HOSTCC_SYSFS_H */
