/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HostCC local response header
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

#ifndef HOSTCC_LOCAL_RESPONSE_H
#define HOSTCC_LOCAL_RESPONSE_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>

#define SLACK_TIME_US 150
#define WORKER_BUDGET 1000000

/* TSC frequency utilities using kernel's mature interfaces */
static inline uint64_t hostcc_tsc_to_microseconds(uint64_t tsc_cycles)
{
    extern unsigned int tsc_khz;
    return (tsc_cycles * 1000ULL) / tsc_khz;
}

static inline uint64_t hostcc_get_elapsed_us(uint64_t start_tsc, uint64_t end_tsc)
{
    return hostcc_tsc_to_microseconds(end_tsc - start_tsc);
}

/* Read TSC using kernel's robust interface */
static inline uint64_t hostcc_read_tsc(void)
{
    return rdtsc_ordered();
}

void update_mba_msr_register(void);
void update_mba_process_scheduler(void);
void init_mba_process_scheduler(void);
void init_mba_msr_register(void);
void increase_mba_val(void);
void decrease_mba_val(void);
void host_local_response(void);

#endif