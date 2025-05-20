// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>

#include "en/cacheflow/cacheflow.h"
#include "en/cacheflow/rq_tracker.h"
#include "net/cacheflow/page_pool.h"

static ssize_t mlx5e_cacheflow_rq_tracker_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
	struct mlx5e_cacheflow *cc = filp->private_data;
	struct mlx5e_cacheflow_rq_tracker *tracker = cc->rq_tracker;
	char kbuf[32];
	size_t len;

	if (!tracker)
		len = scnprintf(kbuf, sizeof(kbuf), "0\n");
	else
		len = scnprintf(kbuf, sizeof(kbuf), "%zd\n", tracker->size);

	return simple_read_from_buffer(buf, count, pos, kbuf, len);
}

static const struct file_operations mlx5e_cacheflow_rq_tracker_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mlx5e_cacheflow_rq_tracker_read,
};

static ssize_t mlx5e_cacheflow_page_pool_stats_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
	struct mlx5e_cacheflow *cc = filp->private_data;
	struct cacheflow_page_pool *pool;
	char kbuf[128];
	size_t len;

	pool = cc->rq.page_pool;

	len = scnprintf(kbuf, sizeof(kbuf),
			"{ \"allocated_pages\": %u, \"ring_pages\": %u, \"array_pages\": %u }\n",
			pool->allocated_pages, pool->ring_pages, pool->array_pages);

	return simple_read_from_buffer(buf, count, pos, kbuf, len);
}

static const struct file_operations mlx5e_cacheflow_page_pool_stats_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mlx5e_cacheflow_page_pool_stats_read,
};

void mlx5e_cacheflow_debugfs_init(struct mlx5e_cacheflow *c)
{
	c->debugfs_dir = debugfs_create_dir("cacheflow", mlx5_debugfs_get_dev_root(c->mdev));

	debugfs_create_file("nic_queue", 0600, c->debugfs_dir, c, &mlx5e_cacheflow_rq_tracker_fops);
	debugfs_create_file("page_pool_stats", 0400, c->debugfs_dir, c, &mlx5e_cacheflow_page_pool_stats_fops);
}

void mlx5e_cacheflow_debugfs_destroy(struct mlx5e_cacheflow *c)
{
	debugfs_remove_recursive(c->debugfs_dir);
}

static ssize_t mlx5e_cacheflow_cqe_fifo_len_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
	struct mlx5e_cacheflow_th *th = filp->private_data;
	struct item_ring *ring = th->cqe_ring;

	char kbuf[32];
	size_t len = scnprintf(kbuf, sizeof(kbuf), "%d\n", max(0, ring->producer.idx - ring->consumer.idx));

	return simple_read_from_buffer(buf, count, pos, kbuf, len);
}

static const struct file_operations mlx5e_cacheflow_cqe_fifo_len_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mlx5e_cacheflow_cqe_fifo_len_read,
};

static ssize_t mlx5e_cacheflow_cqe_fifo_inserted_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
	struct mlx5e_cacheflow_th *th = filp->private_data;
	char kbuf[32];
	size_t len = scnprintf(kbuf, sizeof(kbuf), "%llu\n", th->inserted);

	return simple_read_from_buffer(buf, count, pos, kbuf, len);
}

static const struct file_operations mlx5e_cacheflow_cqe_fifo_inserted_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mlx5e_cacheflow_cqe_fifo_inserted_read,
};

static ssize_t mlx5e_cacheflow_cqe_fifo_missed_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
	struct mlx5e_cacheflow_th *th = filp->private_data;
	char kbuf[32];
	size_t len = scnprintf(kbuf, sizeof(kbuf), "%llu\n", th->missed);

	return simple_read_from_buffer(buf, count, pos, kbuf, len);
}

static const struct file_operations mlx5e_cacheflow_cqe_fifo_missed_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mlx5e_cacheflow_cqe_fifo_missed_read,
};

void mlx5e_cacheflow_th_debugfs_init(struct mlx5e_cacheflow_th *th)
{
	char th_name[16];

	sprintf(th_name, "th_%d", th->cpu);
	th->debugfs_dir = debugfs_create_dir(th_name, th->cacheflow->debugfs_dir);

	debugfs_create_file("fifo_len", 0600, th->debugfs_dir, th, &mlx5e_cacheflow_cqe_fifo_len_fops);
	debugfs_create_file("fifo_inserted", 0600, th->debugfs_dir, th, &mlx5e_cacheflow_cqe_fifo_inserted_fops);
	debugfs_create_file("fifo_missed", 0600, th->debugfs_dir, th, &mlx5e_cacheflow_cqe_fifo_missed_fops);
}

void mlx5e_cacheflow_th_debugfs_destroy(struct mlx5e_cacheflow_th *th)
{
	debugfs_remove_recursive(th->debugfs_dir);
}
