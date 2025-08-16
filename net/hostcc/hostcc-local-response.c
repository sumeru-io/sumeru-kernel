// SPDX-License-Identifier: GPL-2.0
/*
 * HostCC local response implementation
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

#include "hostcc-signals.h"
#include "hostcc-local-response.h"
#include "hostcc-sysfs.h"
#include "intel-cascadelake-params.h"
#include <asm/tsc.h>

/* All measurement variables are now declared in hostcc-signals.h */

/* Local static variables (only used in this file) */
static struct pid *app_pid_struct = NULL;

static void throttle_mba_cores(int cpu)
{
	int err;
	uint64_t assoc_val;
	err = rdmsrl_on_cpu(cpu, PQOS_MSR_ASSOC, &assoc_val);
	if (err) {
		pr_err("HostCC: failed to read MBA MSR register %x, error: %d\n", PQOS_MSR_ASSOC, err);
	}
	err = wrmsrl_on_cpu(cpu, PQOS_MSR_ASSOC, (assoc_val & ~(0x3FFULL)) | hostcc_mba_cos_id);
	if (err) {
		pr_err("HostCC: failed to write MBA MSR register %x, error: %d\n", PQOS_MSR_ASSOC, err);
	}
}

static void unthrottle_mba_cores(int cpu)
{
	int err;
	uint64_t assoc_val;
	err = rdmsrl_on_cpu(cpu, PQOS_MSR_ASSOC, &assoc_val);
	if (err) {
		pr_err("HostCC: failed to read MBA MSR register %x, error: %d\n", PQOS_MSR_ASSOC, err);
	}
	err = wrmsrl_on_cpu(cpu, PQOS_MSR_ASSOC, assoc_val & ~(0x3FFULL));
	if (err) {
		pr_err("HostCC: failed to write MBA MSR register %x, error: %d\n", PQOS_MSR_ASSOC, err);
	}
}

void init_mba_msr_register(void)
{
	int err;
	pr_info("HostCC: init MBA MSR register, use CLOS %d (value: %d)\n", hostcc_mba_cos_id, hostcc_mba_val_high);
	err = wrmsrl_on_cpu(hostcc_mba_level_1_cores[0], PQOS_MSR_MBA_MASK_START + hostcc_mba_cos_id, hostcc_mba_val_high);
	if (err) {
		pr_err("HostCC: failed to write MBA MSR register, error: %d\n", err);
	}
}

void update_mba_msr_register(void)
{
	int i;
	
	/* Reset MBA on all configured cores for all levels */
	for (i = 0; i < hostcc_mba_level_1_core_count; i++) {
		unthrottle_mba_cores(hostcc_mba_level_1_cores[i]);
	}
	for (i = 0; i < hostcc_mba_level_2_core_count; i++) {
		unthrottle_mba_cores(hostcc_mba_level_2_cores[i]);
	}
	for (i = 0; i < hostcc_mba_level_3_core_count; i++) {
		unthrottle_mba_cores(hostcc_mba_level_3_cores[i]);
	}
	wrmsrl_on_cpu(hostcc_mba_level_1_cores[0], PQOS_MSR_MBA_MASK_START + hostcc_mba_cos_id, hostcc_mba_val_low);
}

// helper function to send SIGCONT/SIGSTOP signals to processes
static int send_signal_to_pid(int proc_pid, int signal)
{
	if (proc_pid != 0) {
		trace_printk("HostCC: send %d to MLC PID: %u\n", signal, proc_pid);
		rcu_read_lock();
		app_pid_struct = find_vpid(proc_pid);
		if (app_pid_struct) {
			kill_pid(app_pid_struct, signal, 1);
		}
		rcu_read_unlock();
	}
	return 0;
}

void init_mba_process_scheduler(void)
{}

void update_mba_process_scheduler(void)
{
	WARN_ON(!(latest_mba_val <= 4));
	if (latest_mba_val == 4) {
		send_signal_to_pid(READ_ONCE(hostcc_mem_contender_pid), SIGSTOP);
	} else {
		send_signal_to_pid(READ_ONCE(hostcc_mem_contender_pid), SIGCONT);
	}
}

void increase_mba_val(void)
{
	int i;

	int max_level = 3; // Three MBA levels
	if (hostcc_use_process_scheduler) {
		max_level++; // Allow one more level for SIGSTOP
	}

	if (latest_mba_val >= max_level) {
		return; // Already at maximum throttling
	}

	latest_mba_val++;

	trace_printk("HostCC: increase MBA val to %d\n", latest_mba_val);

	/* Apply MBA throttling to cores progressively by level */
	switch (latest_mba_val) {
	case 1:
		/* Level 1: Throttle all cores in level 1 array */
		for (i = 0; i < hostcc_mba_level_1_core_count; i++) {
			throttle_mba_cores(hostcc_mba_level_1_cores[i]);
		}
		break;
	case 2:
		/* Level 2: Throttle all cores in level 2 array */
		for (i = 0; i < hostcc_mba_level_2_core_count; i++) {
			throttle_mba_cores(hostcc_mba_level_2_cores[i]);
		}
		break;
	case 3:
		/* Level 3: Throttle all cores in level 3 array */
		for (i = 0; i < hostcc_mba_level_3_core_count; i++) {
			throttle_mba_cores(hostcc_mba_level_3_cores[i]);
		}
		break;
	case 4:
		/* Level 4: Use process scheduler (SIGSTOP) */
		if (hostcc_use_process_scheduler) {
			update_mba_process_scheduler();
		}
		break;
	default:
		WARN_ON(!(false));
		break;
	}
}

void decrease_mba_val(void)
{
	uint64_t cur_tsc_val = hostcc_read_tsc();
	/* Use kernel's calibrated TSC frequency for accurate timing */
	uint64_t elapsed_us = hostcc_get_elapsed_us(last_reduced_tsc, cur_tsc_val);
	
	if (elapsed_us < SLACK_TIME_US) {
		return;
	}
	int i;

	if (latest_mba_val <= 0) {
		return; // Already at minimum throttling
	}

	trace_printk("HostCC: decrease MBA val to %d\n", latest_mba_val);

	/* Remove MBA throttling based on current level */
	switch (latest_mba_val) {
	case 1:
		/* Removing level 1: Reset throttling on level 1 cores */
		for (i = 0; i < hostcc_mba_level_1_core_count; i++) {
			unthrottle_mba_cores(hostcc_mba_level_1_cores[i]);
		}
		last_reduced_tsc = hostcc_read_tsc();
		break;
	case 2:
		/* Removing level 2: Reset throttling on level 2 cores */
		for (i = 0; i < hostcc_mba_level_2_core_count; i++) {
			unthrottle_mba_cores(hostcc_mba_level_2_cores[i]);
		}
		last_reduced_tsc = hostcc_read_tsc();
		break;
	case 3:
		/* Removing level 3: Reset throttling on level 3 cores */
		for (i = 0; i < hostcc_mba_level_3_core_count; i++) {
			unthrottle_mba_cores(hostcc_mba_level_3_cores[i]);
		}
		last_reduced_tsc = hostcc_read_tsc();
		break;
	case 4:
		/* Coming down from SIGSTOP level */
		if (hostcc_use_process_scheduler) {
			update_mba_process_scheduler(); // Should send SIGCONT
		}
		last_reduced_tsc = hostcc_read_tsc();
		break;
	default:
		WARN_ON(!(false));
		break;
	}

	latest_mba_val--;
}

void host_local_response(void)
{
	if (hostcc_mode == 0) {
		// Rx side logic
		if ((smoothed_avg_pcie_bw) <
		    (hostcc_target_pcie_thresh << 10)) {
			if (latest_measured_avg_occ_wr >
			    hostcc_target_iio_wr_thresh) {
				increase_mba_val();
			}
		}

		if ((smoothed_avg_pcie_bw) >
		    (hostcc_target_pcie_thresh << 10)) {
			if (latest_measured_avg_occ_wr <
			    hostcc_target_iio_wr_thresh) {
				decrease_mba_val();
			}
		}
	} else {
		//Tx side logic
		if ((smoothed_avg_pcie_bw_rd) <
		    (hostcc_target_pcie_thresh << 10)) {
			if (latest_measured_avg_occ_rd >
			    hostcc_target_iio_rd_thresh) {
				increase_mba_val();
			}
		}

		if ((smoothed_avg_pcie_bw_rd) >
		    (hostcc_target_pcie_thresh << 10)) {
			if (latest_measured_avg_occ_rd <
			    hostcc_target_iio_rd_thresh) {
				decrease_mba_val();
			}
		}
	}
}