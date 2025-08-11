/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HostCC (Host Congestion Control) - Main header file
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

#ifndef HOSTCC_H
#define HOSTCC_H

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
#include <asm/msr.h>


extern struct task_struct *app_pid_task;
extern u64 last_changed_level_tsc;

#define HOSTCC_MODE_RX 0
#define HOSTCC_MODE_TX 1

/* Measurement variables are now declared in hostcc-signals.h */

#endif
