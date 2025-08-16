// SPDX-License-Identifier: GPL-2.0
/*
 * HostCC sysfs interface
 *
 * This file implements the sysfs interface for HostCC parameters
 * that were previously module parameters.
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

#include "hostcc-sysfs.h"

static struct kobject *hostcc_kobj;

/* Original parameters */
int hostcc_mem_contender_pid = 0;
int hostcc_target_pcie_thresh = 84;
int hostcc_target_iio_wr_thresh = 70;
int hostcc_target_iio_rd_thresh = 190;
int hostcc_enable_network_response = 1;
int hostcc_enable_local_response = 1;
int hostcc_mode = 0; /* 0 = Rx, 1 = Tx */

/* Runtime control parameters */
int hostcc_enable = 0; /* Global enable/disable for HostCC - start disabled */
int hostcc_enable_logging = 1; /* Enable/disable logging */

char hostcc_nic_interface[16] = "ens2f1";
int hostcc_nic_local_socket = 0;
int hostcc_nic_iio_stack = 2;
/* MBA core lists for each throttling level */
int hostcc_mba_level_1_cores[HOSTCC_MAX_MBA_CORES] = {0};
int hostcc_mba_level_1_core_count = 1;
int hostcc_mba_level_2_cores[HOSTCC_MAX_MBA_CORES] = {0};
int hostcc_mba_level_2_core_count = 1;
int hostcc_mba_level_3_cores[HOSTCC_MAX_MBA_CORES] = {0};
int hostcc_mba_level_3_core_count = 1;
int hostcc_mba_val_high = 90;
int hostcc_mba_val_low = 0;
int hostcc_mba_cos_id = 1;
int hostcc_use_process_scheduler = 1;
int hostcc_iio_core = 48;
int hostcc_iio_logging = 0;
int hostcc_pcie_core = 50;
int hostcc_pcie_logging = 1;
int hostcc_ecn_logging = 0;
int hostcc_log_size = 10000;

/* Sysfs attribute show/store functions */
static ssize_t target_pid_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hostcc_mem_contender_pid);
}

static ssize_t target_pid_store(struct kobject *kobj,
				struct kobj_attribute *attr, const char *buf,
				size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1) {
		hostcc_mem_contender_pid = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t target_pcie_thresh_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hostcc_target_pcie_thresh);
}

static ssize_t target_pcie_thresh_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1) {
		hostcc_target_pcie_thresh = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t target_iio_wr_thresh_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hostcc_target_iio_wr_thresh);
}

static ssize_t target_iio_wr_thresh_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1) {
		hostcc_target_iio_wr_thresh = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t target_iio_rd_thresh_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hostcc_target_iio_rd_thresh);
}

static ssize_t target_iio_rd_thresh_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1) {
		hostcc_target_iio_rd_thresh = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t enable_network_response_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sprintf(buf, "%d\n", hostcc_enable_network_response);
}

static ssize_t enable_network_response_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1 && (val == 0 || val == 1)) {
		hostcc_enable_network_response = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t enable_local_response_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "%d\n", hostcc_enable_local_response);
}

static ssize_t enable_local_response_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1 && (val == 0 || val == 1)) {
		hostcc_enable_local_response = val;
		return count;
	}
	return -EINVAL;
}

static ssize_t hostcc_mode_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hostcc_mode);
}

static ssize_t hostcc_mode_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count)
{
	int val;
	if (sscanf(buf, "%d", &val) == 1 && (val == 0 || val == 1)) {
		hostcc_mode = val;
		return count;
	}
	return -EINVAL;
}

/* Hardware configuration parameter show/store functions */
static ssize_t nic_interface_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", hostcc_nic_interface);
}

static ssize_t nic_interface_store(struct kobject *kobj,
				   struct kobj_attribute *attr, const char *buf,
				   size_t count)
{
	char interface[16];
	if (sscanf(buf, "%15s", interface) == 1) {
		strncpy(hostcc_nic_interface, interface,
			sizeof(hostcc_nic_interface) - 1);
		hostcc_nic_interface[sizeof(hostcc_nic_interface) - 1] = '\0';
		return count;
	}
	return -EINVAL;
}

#define HOSTCC_ATTR_SIMPLE(name, var)                                      \
	static ssize_t name##_show(struct kobject *kobj,                   \
				   struct kobj_attribute *attr, char *buf) \
	{                                                                  \
		return sprintf(buf, "%d\n", var);                          \
	}                                                                  \
	static ssize_t name##_store(struct kobject *kobj,                  \
				    struct kobj_attribute *attr,           \
				    const char *buf, size_t count)         \
	{                                                                  \
		int val;                                                   \
		if (sscanf(buf, "%d", &val) == 1) {                        \
			var = val;                                         \
			return count;                                      \
		}                                                          \
		return -EINVAL;                                            \
	}

HOSTCC_ATTR_SIMPLE(nic_local_socket, hostcc_nic_local_socket)
HOSTCC_ATTR_SIMPLE(nic_iio_stack, hostcc_nic_iio_stack)

/* MBA core list attribute functions for each level */
#define MBA_LEVEL_ATTR_FUNCTIONS(level_num) \
static ssize_t mba_level_##level_num##_cores_show(struct kobject *kobj, \
						   struct kobj_attribute *attr, char *buf) \
{ \
	int i, len = 0; \
	for (i = 0; i < hostcc_mba_level_##level_num##_core_count; i++) { \
		len += sprintf(buf + len, "%d", hostcc_mba_level_##level_num##_cores[i]); \
		if (i < hostcc_mba_level_##level_num##_core_count - 1) \
			len += sprintf(buf + len, ","); \
	} \
	len += sprintf(buf + len, "\n"); \
	return len; \
} \
\
static ssize_t mba_level_##level_num##_cores_store(struct kobject *kobj, \
						    struct kobj_attribute *attr, \
						    const char *buf, size_t count) \
{ \
	char *str, *token; \
	int cores[HOSTCC_MAX_MBA_CORES]; \
	int core_count = 0; \
	int val; \
\
	str = kstrdup(buf, GFP_KERNEL); \
	if (!str) \
		return -ENOMEM; \
\
	/* Parse comma-separated list of core IDs */ \
	while ((token = strsep(&str, ",\n ")) != NULL) { \
		if (*token == '\0') \
			continue; \
		if (kstrtoint(token, 10, &val) == 0) { \
			if (core_count < HOSTCC_MAX_MBA_CORES && val >= 0) { \
				cores[core_count++] = val; \
			} \
		} \
	} \
\
	if (core_count > 0) { \
		int i; \
		/* Clear the array first */ \
		memset(hostcc_mba_level_##level_num##_cores, 0, \
		       sizeof(hostcc_mba_level_##level_num##_cores)); \
		/* Copy new values */ \
		for (i = 0; i < core_count; i++) { \
			hostcc_mba_level_##level_num##_cores[i] = cores[i]; \
		} \
		hostcc_mba_level_##level_num##_core_count = core_count; \
	} \
\
	kfree(str); \
	return count; \
}

/* Generate the attribute functions for all three levels */
MBA_LEVEL_ATTR_FUNCTIONS(1)
MBA_LEVEL_ATTR_FUNCTIONS(2)
MBA_LEVEL_ATTR_FUNCTIONS(3)
HOSTCC_ATTR_SIMPLE(mba_val_high, hostcc_mba_val_high)
HOSTCC_ATTR_SIMPLE(mba_val_low, hostcc_mba_val_low)
HOSTCC_ATTR_SIMPLE(mba_cos_id, hostcc_mba_cos_id)
HOSTCC_ATTR_SIMPLE(use_process_scheduler, hostcc_use_process_scheduler)
HOSTCC_ATTR_SIMPLE(iio_core, hostcc_iio_core)
HOSTCC_ATTR_SIMPLE(iio_logging, hostcc_iio_logging)
HOSTCC_ATTR_SIMPLE(pcie_core, hostcc_pcie_core)
HOSTCC_ATTR_SIMPLE(pcie_logging, hostcc_pcie_logging)
HOSTCC_ATTR_SIMPLE(ecn_logging, hostcc_ecn_logging)
HOSTCC_ATTR_SIMPLE(log_size, hostcc_log_size)
/* Custom enable/disable handler */
static ssize_t enable_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hostcc_enable);
}

static ssize_t enable_store(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	int val, ret;

	if (sscanf(buf, "%d", &val) == 1 && (val == 0 || val == 1)) {
		ret = hostcc_set_enable(val);
		if (ret) {
			pr_err("HostCC: Failed to %s: %d\n",
			       val ? "enable" : "disable", ret);
			return ret;
		}
		return count;
	}
	return -EINVAL;
}

HOSTCC_ATTR_SIMPLE(enable_logging, hostcc_enable_logging)

/* Sysfs attributes */
static struct kobj_attribute target_pid_attr =
	__ATTR(target_pid, 0644, target_pid_show, target_pid_store);
static struct kobj_attribute target_pcie_thresh_attr =
	__ATTR(target_pcie_thresh, 0644, target_pcie_thresh_show,
	       target_pcie_thresh_store);
static struct kobj_attribute target_iio_wr_thresh_attr =
	__ATTR(target_iio_wr_thresh, 0644, target_iio_wr_thresh_show,
	       target_iio_wr_thresh_store);
static struct kobj_attribute target_iio_rd_thresh_attr =
	__ATTR(target_iio_rd_thresh, 0644, target_iio_rd_thresh_show,
	       target_iio_rd_thresh_store);
static struct kobj_attribute enable_network_response_attr =
	__ATTR(enable_network_response, 0644, enable_network_response_show,
	       enable_network_response_store);
static struct kobj_attribute enable_local_response_attr =
	__ATTR(enable_local_response, 0644, enable_local_response_show,
	       enable_local_response_store);

static struct kobj_attribute mode_attr =
	__ATTR(mode, 0644, hostcc_mode_show, hostcc_mode_store);

/* Hardware configuration attributes */
static struct kobj_attribute nic_interface_attr =
	__ATTR(nic_interface, 0644, nic_interface_show, nic_interface_store);
static struct kobj_attribute nic_local_socket_attr = __ATTR(
	nic_local_socket, 0644, nic_local_socket_show, nic_local_socket_store);
static struct kobj_attribute nic_iio_stack_attr =
	__ATTR(nic_iio_stack, 0644, nic_iio_stack_show, nic_iio_stack_store);
static struct kobj_attribute mba_level_1_cores_attr =
	__ATTR(mba_level_1_cores, 0644, mba_level_1_cores_show, mba_level_1_cores_store);
static struct kobj_attribute mba_level_2_cores_attr =
	__ATTR(mba_level_2_cores, 0644, mba_level_2_cores_show, mba_level_2_cores_store);
static struct kobj_attribute mba_level_3_cores_attr =
	__ATTR(mba_level_3_cores, 0644, mba_level_3_cores_show, mba_level_3_cores_store);
static struct kobj_attribute mba_val_high_attr =
	__ATTR(mba_val_high, 0644, mba_val_high_show, mba_val_high_store);
static struct kobj_attribute mba_val_low_attr =
	__ATTR(mba_val_low, 0644, mba_val_low_show, mba_val_low_store);
static struct kobj_attribute mba_cos_id_attr =
	__ATTR(mba_cos_id, 0644, mba_cos_id_show, mba_cos_id_store);
static struct kobj_attribute use_process_scheduler_attr =
	__ATTR(use_process_scheduler, 0644, use_process_scheduler_show,
	       use_process_scheduler_store);
static struct kobj_attribute iio_core_attr =
	__ATTR(iio_core, 0644, iio_core_show, iio_core_store);
static struct kobj_attribute iio_logging_attr =
	__ATTR(iio_logging, 0644, iio_logging_show, iio_logging_store);
static struct kobj_attribute pcie_core_attr =
	__ATTR(pcie_core, 0644, pcie_core_show, pcie_core_store);
static struct kobj_attribute pcie_logging_attr =
	__ATTR(pcie_logging, 0644, pcie_logging_show, pcie_logging_store);
static struct kobj_attribute ecn_logging_attr =
	__ATTR(ecn_logging, 0644, ecn_logging_show, ecn_logging_store);
static struct kobj_attribute log_size_attr =
	__ATTR(log_size, 0644, log_size_show, log_size_store);
static struct kobj_attribute enable_attr =
	__ATTR(enable, 0644, enable_show, enable_store);
static struct kobj_attribute enable_logging_attr =
	__ATTR(enable_logging, 0644, enable_logging_show, enable_logging_store);

static struct attribute *hostcc_attrs[] = {
	/* Core parameters */
	&target_pid_attr.attr,
	&target_pcie_thresh_attr.attr,
	&target_iio_wr_thresh_attr.attr,
	&target_iio_rd_thresh_attr.attr,
	&enable_network_response_attr.attr,
	&enable_local_response_attr.attr,
	&mode_attr.attr,
	&nic_interface_attr.attr,
	&nic_local_socket_attr.attr,
	&nic_iio_stack_attr.attr,
	&mba_level_1_cores_attr.attr,
	&mba_level_2_cores_attr.attr,
	&mba_level_3_cores_attr.attr,
	&mba_val_high_attr.attr,
	&mba_val_low_attr.attr,
	&mba_cos_id_attr.attr,
	&use_process_scheduler_attr.attr,
	&iio_core_attr.attr,
	&iio_logging_attr.attr,
	&pcie_core_attr.attr,
	&pcie_logging_attr.attr,
	&ecn_logging_attr.attr,
	&log_size_attr.attr,
	&enable_attr.attr,
	&enable_logging_attr.attr,
	NULL,
};

static struct attribute_group hostcc_attr_group = {
	.attrs = hostcc_attrs,
};

int hostcc_sysfs_init(void)
{
	int ret;

	hostcc_kobj = kobject_create_and_add("hostcc", kernel_kobj);
	if (!hostcc_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(hostcc_kobj, &hostcc_attr_group);
	if (ret) {
		kobject_put(hostcc_kobj);
		return ret;
	}

	return 0;
}

void hostcc_sysfs_cleanup(void)
{
	if (hostcc_kobj) {
		sysfs_remove_group(hostcc_kobj, &hostcc_attr_group);
		kobject_put(hostcc_kobj);
	}
}
