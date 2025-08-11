/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HostCC signals header
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

#ifndef HOSTCC_SIGNALS_H
#define HOSTCC_SIGNALS_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/threads.h>
#include <linux/delay.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <asm/msr.h>  /* For rdtsc_ordered() */

/* IIO and PCIe measurement variables */
extern uint64_t last_changed_level_tsc;
extern uint64_t cur_rdtsc_iio_rd;
extern uint64_t latest_avg_occ_rd;
extern uint64_t smoothed_avg_occ_rd;
extern uint64_t latest_time_delta_iio_rd_ns;
extern uint64_t cur_rdtsc_iio_wr;
extern uint64_t latest_avg_occ_wr;
extern uint64_t smoothed_avg_occ_wr;
extern uint64_t latest_time_delta_iio_wr_ns;
extern uint64_t cur_rdtsc_mba;
extern uint64_t latest_time_delta_mba_ns;
extern uint32_t latest_avg_pcie_bw;
extern uint32_t latest_avg_pcie_bw_rd;
extern uint32_t app_pid;
extern uint64_t last_reduced_tsc;

/* Netfilter measurement variables */
extern uint64_t latest_measured_avg_occ_wr_nf;
extern uint64_t latest_measured_avg_occ_rd_nf;
extern uint64_t latest_time_delta_nf_ns;
extern uint32_t latest_datagram_len;
extern uint64_t tsc_sample_nf;
extern uint64_t cur_rdtsc_nf;
extern uint64_t prev_rdtsc_nf;

/* Variables defined in hostcc-core.c */
extern uint32_t latest_mba_val;
extern uint32_t smoothed_avg_pcie_bw;
extern uint32_t smoothed_avg_pcie_bw_rd;
extern uint32_t latest_measured_avg_occ_wr;
extern uint32_t latest_measured_avg_occ_rd;
extern struct task_struct *app_pid_task;

/* Function prototypes */
void update_iio_rd_occ_ctl_reg(void);
void sample_iio_rd_occ_counter(int c);
void sample_iio_rd_time_counter(void);
void sample_counters_iio_rd(int c);
void update_iio_rd_occ(void);
void update_iio_wr_occ_ctl_reg(void);
void sample_iio_wr_occ_counter(int c);
void sample_iio_wr_time_counter(void);
void sample_counters_iio_wr(int c);
void update_iio_wr_occ(void);
void sample_pcie_bw_counter(int c);
void sample_mba_time_counter(void);
void sample_counters_pcie_bw(int c);
void update_pcie_bw(void);

#endif