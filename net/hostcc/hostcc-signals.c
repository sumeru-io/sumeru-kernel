// SPDX-License-Identifier: GPL-2.0
/*
 * HostCC signals implementation
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
#include "intel-cascadelake-params.h"
#include "hostcc-sysfs.h"
#include "hostcc.h"

/* Variables used only by signals module */
uint64_t last_changed_level_tsc = 0;
uint64_t cur_rdtsc_iio_rd = 0;
uint64_t latest_avg_occ_rd = 0;
uint64_t smoothed_avg_occ_rd = 0;
uint64_t latest_time_delta_iio_rd_ns = 0;
uint64_t cur_rdtsc_iio_wr = 0;
uint64_t latest_avg_occ_wr = 0;
uint64_t smoothed_avg_occ_wr = 0;
uint64_t latest_time_delta_iio_wr_ns = 0;
uint64_t cur_rdtsc_mba = 0;
uint64_t latest_time_delta_mba_ns = 0;
uint32_t latest_avg_pcie_bw = 0;
uint32_t latest_avg_pcie_bw_rd = 0;
uint32_t app_pid = 0;
uint64_t last_reduced_tsc = 0;

/* Netfilter measurement variables */
uint64_t latest_measured_avg_occ_wr_nf = 0;
uint64_t latest_measured_avg_occ_rd_nf = 0;
uint64_t latest_time_delta_nf_ns = 0;
uint32_t latest_datagram_len = 0;
uint64_t tsc_sample_nf = 0;
uint64_t cur_rdtsc_nf = 0;
uint64_t prev_rdtsc_nf = 0;

uint64_t prev_rdtsc_mba = 0;
uint64_t tsc_sample_mba = 0;
uint64_t cur_cum_frc = 0;
uint64_t prev_cum_frc = 0;
uint64_t cum_frc_sample = 0;
uint64_t cur_cum_frc_rd = 0;
uint64_t prev_cum_frc_rd = 0;
uint64_t cum_frc_rd_sample = 0;
uint64_t prev_rdtsc_iio_wr = 0;
uint64_t prev_cum_occ_wr = 0;
uint64_t cur_cum_occ_wr = 0;
uint64_t tsc_sample_iio_wr = 0;
uint64_t cum_occ_sample_wr = 0;
uint64_t cum_occ_sample_rd;
uint64_t prev_cum_occ_rd;
uint64_t cur_cum_occ_rd;
uint64_t prev_rdtsc_iio_rd = 0;
uint64_t tsc_sample_iio_rd = 0;

// IIO Read occupancy logic (obtained via CHA counters)
void update_iio_rd_occ_ctl_reg(void)
{
	//program the desired CTL register to read the corresponding CTR value
	uint64_t msr_num;
	msr_num = IIO_MSR_PMON_CTL_BASE + (0x20 * hostcc_nic_iio_stack) +
		  IIO_RD_COUNTER_OFFSET;
	uint32_t low = IIO_OCC_VAL & 0xFFFFFFFF;
	uint32_t high = IIO_OCC_VAL >> 32;
	wrmsr_on_cpu(hostcc_iio_core, msr_num, low, high);
}

void sample_iio_rd_occ_counter(int c)
{
	uint64_t msr_num;
	uint32_t low = 0;
	uint32_t high = 0;
	msr_num = IIO_MSR_PMON_CTR_BASE + (0x20 * hostcc_nic_iio_stack) +
		  IIO_RD_COUNTER_OFFSET;
	rdmsr_on_cpu(c, msr_num, &low, &high);
	cum_occ_sample_rd = ((uint64_t)high << 32) | low;
	prev_cum_occ_rd = cur_cum_occ_rd;
	cur_cum_occ_rd = cum_occ_sample_rd;
}

void sample_iio_rd_time_counter(void)
{
	tsc_sample_iio_rd = hostcc_read_tsc();
	prev_rdtsc_iio_rd = cur_rdtsc_iio_rd;
	cur_rdtsc_iio_rd = tsc_sample_iio_rd;
}

void sample_counters_iio_rd(int c)
{
	//first sample occupancy
	sample_iio_rd_occ_counter(c);
	//sample time at the last
	sample_iio_rd_time_counter();
	return;
}

void update_iio_rd_occ(void)
{
	/* Convert TSC cycles to nanoseconds using kernel's TSC frequency */
	extern unsigned int tsc_khz;
	latest_time_delta_iio_rd_ns =
		((cur_rdtsc_iio_rd - prev_rdtsc_iio_rd) * 935000ULL) / tsc_khz;
	if (latest_time_delta_iio_rd_ns > 0) {
		latest_avg_occ_rd = (cur_cum_occ_rd - prev_cum_occ_rd) /
				    (latest_time_delta_iio_rd_ns);
		// ((occ[i] - occ[i-1]) / (((time_us[i+1] - time_us[i])) * 1e-6 * freq));
		// IIO counter operates at thee frequency of 1000MHz
		if (latest_avg_occ_rd >= 0) {
			smoothed_avg_occ_rd = ((7 * smoothed_avg_occ_rd) +
					       (latest_avg_occ_rd << 10)) >>
					      3;
		}
	}
}

// IIO Write occupancy logic
void update_iio_wr_occ_ctl_reg(void)
{
	//program the desired CTL register to read the corresponding CTR value
	uint64_t msr_num;
	msr_num = IRP_MSR_PMON_CTL_BASE + (0x20 * hostcc_nic_iio_stack) +
		  IIO_WR_COUNTER_OFFSET;
	uint32_t low = IRP_OCC_VAL & 0xFFFFFFFF;
	uint32_t high = (uint64_t)IRP_OCC_VAL >> 32;
	wrmsr_on_cpu(hostcc_iio_core, msr_num, low, high);
}

void sample_iio_wr_occ_counter(int c)
{
	uint64_t msr_num;
	uint32_t low = 0;
	uint32_t high = 0;
	msr_num = IRP_MSR_PMON_CTR_BASE + (0x20 * hostcc_nic_iio_stack) +
		  IIO_WR_COUNTER_OFFSET;
	rdmsr_on_cpu(c, msr_num, &low, &high);
	cum_occ_sample_wr = ((uint64_t)high << 32) | low;
	prev_cum_occ_wr = cur_cum_occ_wr;
	cur_cum_occ_wr = cum_occ_sample_wr;
}

void sample_iio_wr_time_counter(void)
{
	tsc_sample_iio_wr = hostcc_read_tsc();
	prev_rdtsc_iio_wr = cur_rdtsc_iio_wr;
	cur_rdtsc_iio_wr = tsc_sample_iio_wr;
}

void sample_counters_iio_wr(int c)
{
	//first sample occupancy
	sample_iio_wr_occ_counter(c);
	//sample time at the last
	sample_iio_wr_time_counter();
	return;
}

void update_iio_wr_occ(void)
{
	/* Convert TSC cycles to nanoseconds using kernel's TSC frequency */
	latest_time_delta_iio_wr_ns =
		((cur_rdtsc_iio_wr - prev_rdtsc_iio_wr) * 465000ULL) / tsc_khz;
	if (latest_time_delta_iio_wr_ns > 0) {
		latest_avg_occ_wr = (cur_cum_occ_wr - prev_cum_occ_wr) /
				    (latest_time_delta_iio_wr_ns);
		// ((occ[i] - occ[i-1]) / (((time_us[i+1] - time_us[i])) * 1e-6 * freq));
		// IRP counter operates at the frequency of 500MHz
		if (latest_avg_occ_wr > 0) {
			smoothed_avg_occ_wr = ((7 * smoothed_avg_occ_wr) +
					       (latest_avg_occ_wr << 10)) >>
					      3;
		}
	}
}

// PCIe bandwidth sampling and MBA update logic

void sample_pcie_bw_counter(int c)
{
	uint32_t low = 0;
	uint32_t high = 0;
	uint64_t msr_num;
	if (hostcc_mode == HOSTCC_MODE_RX) {
		msr_num = IIO_PCIE_1_PORT_0_BW_IN;
		rdmsr_on_cpu(c, msr_num, &low, &high);
		cum_frc_sample = ((uint64_t)high << 32) | low;
		prev_cum_frc = cur_cum_frc;
		cur_cum_frc = cum_frc_sample;
	} else {
		msr_num = IIO_PCIE_1_PORT_0_BW_OUT;
		rdmsr_on_cpu(c, msr_num, &low, &high);
		cum_frc_rd_sample = ((uint64_t)high << 32) | low;
		prev_cum_frc_rd = cur_cum_frc_rd;
		cur_cum_frc_rd = cum_frc_rd_sample;
	}
}

void sample_mba_time_counter(void)
{
	tsc_sample_mba = hostcc_read_tsc();
	prev_rdtsc_mba = cur_rdtsc_mba;
	cur_rdtsc_mba = tsc_sample_mba;
}

void sample_counters_pcie_bw(int c)
{
	//first pcie bw
	sample_pcie_bw_counter(c);
	//sample time at the last
	sample_mba_time_counter();
	return;
}

void update_pcie_bw(void)
{
	/* Convert TSC cycles to nanoseconds using kernel's TSC frequency */
	latest_time_delta_mba_ns = ((cur_rdtsc_mba - prev_rdtsc_mba) * 935000ULL) / tsc_khz;
	if (latest_time_delta_mba_ns > 0) {
		// latest_measured_avg_pcie_bw = (uint32_t)((((float)(cur_cum_frc - prev_cum_frc)) / ((float)(latest_time_delta_ns)) ) * 32);
		if (hostcc_mode == HOSTCC_MODE_RX) {
			latest_avg_pcie_bw = (cur_cum_frc - prev_cum_frc) /
					     (latest_time_delta_mba_ns >> 5);
			if (latest_avg_pcie_bw < 150) {
				smoothed_avg_pcie_bw =
					((255 * smoothed_avg_pcie_bw) +
					 (latest_avg_pcie_bw << 10)) >>
					8;
			}
		} else {
			latest_avg_pcie_bw_rd =
				(cur_cum_frc_rd - prev_cum_frc_rd) /
				(latest_time_delta_mba_ns >> 5);
			if (latest_avg_pcie_bw_rd < 150) {
				smoothed_avg_pcie_bw_rd =
					((255 * smoothed_avg_pcie_bw_rd) +
					 (latest_avg_pcie_bw_rd << 10)) >>
					8;
			}
		}
	}
}
