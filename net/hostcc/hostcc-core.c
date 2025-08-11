// SPDX-License-Identifier: GPL-2.0
/*
 * HostCC (Host Congestion Control)
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
 *
 * This file implements the core HostCC functionality as a built-in
 * kernel subsystem for monitoring system congestion and providing
 * network feedback through ECN marking and local response mechanisms.
 */

#include "hostcc.h"
#include "hostcc-signals.h"
#include "hostcc-local-response.h"
#include "hostcc-network-response.h"
#include "hostcc-logging.h"
#include "hostcc-sysfs.h"

/* Global variables shared across HostCC modules */
uint32_t latest_mba_val = 0;
uint32_t smoothed_avg_pcie_bw = 0;
uint32_t smoothed_avg_pcie_bw_rd = 0;
uint32_t latest_measured_avg_occ_wr = 0;
uint32_t latest_measured_avg_occ_rd = 0;
struct task_struct *app_pid_task = NULL;

static struct workqueue_struct *poll_iio_queue, *poll_pcie_queue;
static struct work_struct poll_iio, poll_pcie;

static void poll_iio_init(void)
{
	/* Initialize the log */
	pr_info("HostCC: Starting IIO Occupancy measurement");
	if (hostcc_mode == HOSTCC_MODE_RX) {
		init_iio_wr_log();
		update_iio_wr_occ_ctl_reg();
	} else {
		init_iio_rd_log();
		update_iio_rd_occ_ctl_reg();
	}
}

static void poll_iio_exit(void)
{
	/* Dump log info */
	pr_info("HostCC: Ending IIO Occupancy measurement");
	if (poll_iio_queue) {
		flush_workqueue(poll_iio_queue);
		destroy_workqueue(poll_iio_queue);
	}
	if (hostcc_mode == HOSTCC_MODE_RX) {
		if (hostcc_iio_logging) {
			dump_iio_wr_log();
		}
	} else {
		if (hostcc_iio_logging) {
			dump_iio_rd_log();
		}
	}
}

static void thread_fun_poll_iio(struct work_struct *work)
{
	int cpu = hostcc_iio_core;
	uint32_t budget = WORKER_BUDGET;

	while (budget) {
		if (hostcc_mode == HOSTCC_MODE_RX) {
			sample_counters_iio_wr(cpu); /* sample counters */
			update_iio_wr_occ(); /* update occupancy value */
			if (hostcc_enable_logging && hostcc_iio_logging) {
				update_log_iio_wr(cpu); /* update the log */
			}
		} else {
			sample_counters_iio_rd(cpu); /* sample counters */
			update_iio_rd_occ(); /* update occupancy value */
			if (hostcc_enable_logging && hostcc_iio_logging) {
				update_log_iio_rd(cpu); /* update the log */
			}
		}
		budget--;
	}

	if (hostcc_enable) {
		queue_work_on(cpu, poll_iio_queue, &poll_iio);
	}
}

static void poll_pcie_init(void)
{
	if (hostcc_use_process_scheduler) {
		init_mba_process_scheduler();
	}
	/* Initialize the log */
	pr_info("HostCC: Starting PCIe Bandwidth Measurement");
	init_pcie_log();
}

static void poll_pcie_exit(void)
{
	/* Dump log info */
	pr_info("HostCC: Ending PCIe Bandwidth Measurement");
	if (poll_pcie_queue) {
		flush_workqueue(poll_pcie_queue);
		destroy_workqueue(poll_pcie_queue);
	}
	if (latest_mba_val > 0) {
		latest_mba_val = 0;
		update_mba_msr_register();
		if (hostcc_use_process_scheduler) {
			update_mba_process_scheduler();
		}
	}
	if (hostcc_pcie_logging) {
		dump_pcie_log();
	}
}

static void thread_fun_poll_pcie(struct work_struct *work)
{
	int cpu = hostcc_pcie_core;
	uint32_t budget = WORKER_BUDGET;

	while (budget) {
		sample_counters_pcie_bw(cpu);
		update_pcie_bw();

		if (hostcc_mode == HOSTCC_MODE_RX) {
			latest_measured_avg_occ_wr =
				smoothed_avg_occ_wr >>
				10; /* consistent IIO occupancy value */
		} else {
			latest_measured_avg_occ_rd =
				smoothed_avg_occ_rd >>
				10; /* consistent IIO occupancy value */
		}

		if (hostcc_enable_local_response) {
			host_local_response();
		}

		if (hostcc_enable_logging && hostcc_pcie_logging) {
			update_log_pcie(cpu);
		}

		budget--;
	}

	if (hostcc_enable) {
		queue_work_on(cpu, poll_pcie_queue, &poll_pcie);
	}
}

static int __init hostcc_init(void)
{
	int ret;

	pr_info("HostCC: Initializing Host Congestion Control\n");

	/* Initialize sysfs interface */
	ret = hostcc_sysfs_init();
	if (ret) {
		pr_err("HostCC: Failed to initialize sysfs interface\n");
		return ret;
	}

	/* Start IIO occupancy measurement */
	poll_iio_queue =
		alloc_workqueue("hostcc_iio", WQ_HIGHPRI | WQ_CPU_INTENSIVE, 0);
	if (!poll_iio_queue) {
		pr_err("HostCC: Failed to create IIO workqueue\n");
		ret = -ENOMEM;
		goto err_sysfs;
	}

	INIT_WORK(&poll_iio, thread_fun_poll_iio);
	poll_iio_init();
	queue_work_on(hostcc_iio_core, poll_iio_queue, &poll_iio);

	/* Start PCIe bandwidth measurement */
	poll_pcie_queue = alloc_workqueue("hostcc_pcie",
					  WQ_HIGHPRI | WQ_CPU_INTENSIVE, 0);
	if (!poll_pcie_queue) {
		pr_err("HostCC: Failed to create PCIe workqueue\n");
		ret = -ENOMEM;
		goto err_iio;
	}

	INIT_WORK(&poll_pcie, thread_fun_poll_pcie);
	poll_pcie_init();
	queue_work_on(hostcc_pcie_core, poll_pcie_queue, &poll_pcie);

	/* Start ECN marking */
	ret = nf_init();
	if (ret) {
		pr_err("HostCC: Failed to initialize netfilter hooks\n");
		goto err_pcie;
	}

	pr_info("HostCC: Initialization completed successfully\n");
	return 0;

err_pcie:
	poll_pcie_exit();
err_iio:
	poll_iio_exit();
err_sysfs:
	hostcc_sysfs_cleanup();
	return ret;
}

static void __exit hostcc_exit(void)
{
	pr_info("HostCC: Shutting down Host Congestion Control\n");

	/* Signal termination to worker threads */
	hostcc_enable_logging = 0;
	msleep(5000); /* Allow logging to finish */
	hostcc_enable = 0;

	/* Clean up netfilter hooks */
	nf_exit();

	/* Stop worker threads and clean up */
	poll_iio_exit();
	poll_pcie_exit();

	/* Clean up sysfs interface */
	hostcc_sysfs_cleanup();

	pr_info("HostCC: Shutdown completed\n");
}

/* For built-in subsystems, we use subsys_initcall instead of module_init */
subsys_initcall(hostcc_init);
/* Exit function is only called during shutdown, no module_exit needed */
