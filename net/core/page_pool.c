/* SPDX-License-Identifier: GPL-2.0
 *
 * page_pool.c
 *	Author:	Jesper Dangaard Brouer <netoptimizer@brouer.com>
 *	Copyright (C) 2016 Red Hat, Inc.
 */

#include <linux/error-injection.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>

#include <net/netdev_rx_queue.h>
#include <net/page_pool/helpers.h>
#include <net/xdp.h>

#ifdef CONFIG_NET_CACHEFLOW
#include <net/cacheflow.h>
#endif

#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/page-flags.h>
#include <linux/mm.h> /* for put_page() */
#include <linux/poison.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>

#ifdef CONFIG_NET_CACHEFLOW
#include <linux/proc_fs.h>
#endif

#include <trace/events/page_pool.h>

#include "mp_dmabuf_devmem.h"
#include "netmem_priv.h"
#include "page_pool_priv.h"

DEFINE_STATIC_KEY_FALSE(page_pool_mem_providers);

#define DEFER_TIME (msecs_to_jiffies(1000))
#define DEFER_WARN_INTERVAL (60 * HZ)

#define BIAS_MAX	(LONG_MAX >> 1)

#ifdef CONFIG_PAGE_POOL_STATS
static DEFINE_PER_CPU(struct page_pool_recycle_stats, pp_system_recycle_stats);

/* alloc_stat_inc is intended to be used in softirq context */
#define alloc_stat_inc(pool, __stat)	(pool->alloc_stats.__stat++)
/* recycle_stat_inc is safe to use when preemption is possible. */
#define recycle_stat_inc(pool, __stat)							\
	do {										\
		struct page_pool_recycle_stats __percpu *s = pool->recycle_stats;	\
		this_cpu_inc(s->__stat);						\
	} while (0)

#define recycle_stat_add(pool, __stat, val)						\
	do {										\
		struct page_pool_recycle_stats __percpu *s = pool->recycle_stats;	\
		this_cpu_add(s->__stat, val);						\
	} while (0)

static const char pp_stats[][ETH_GSTRING_LEN] = {
	"rx_pp_alloc_fast",
	"rx_pp_alloc_slow",
	"rx_pp_alloc_slow_ho",
	"rx_pp_alloc_empty",
	"rx_pp_alloc_refill",
	"rx_pp_alloc_waive",
	"rx_pp_recycle_cached",
	"rx_pp_recycle_cache_full",
	"rx_pp_recycle_ring",
	"rx_pp_recycle_ring_full",
	"rx_pp_recycle_released_ref",
};

/**
 * page_pool_get_stats() - fetch page pool stats
 * @pool:	pool from which page was allocated
 * @stats:	struct page_pool_stats to fill in
 *
 * Retrieve statistics about the page_pool. This API is only available
 * if the kernel has been configured with ``CONFIG_PAGE_POOL_STATS=y``.
 * A pointer to a caller allocated struct page_pool_stats structure
 * is passed to this API which is filled in. The caller can then report
 * those stats to the user (perhaps via ethtool, debugfs, etc.).
 */
bool page_pool_get_stats(const struct page_pool *pool,
			 struct page_pool_stats *stats)
{
	int cpu = 0;

	if (!stats)
		return false;

	/* The caller is responsible to initialize stats. */
	stats->alloc_stats.fast += pool->alloc_stats.fast;
	stats->alloc_stats.slow += pool->alloc_stats.slow;
	stats->alloc_stats.slow_high_order += pool->alloc_stats.slow_high_order;
	stats->alloc_stats.empty += pool->alloc_stats.empty;
	stats->alloc_stats.refill += pool->alloc_stats.refill;
	stats->alloc_stats.waive += pool->alloc_stats.waive;

	for_each_possible_cpu(cpu) {
		const struct page_pool_recycle_stats *pcpu =
			per_cpu_ptr(pool->recycle_stats, cpu);

		stats->recycle_stats.cached += pcpu->cached;
		stats->recycle_stats.cache_full += pcpu->cache_full;
		stats->recycle_stats.ring += pcpu->ring;
		stats->recycle_stats.ring_full += pcpu->ring_full;
		stats->recycle_stats.released_refcnt += pcpu->released_refcnt;
	}

	return true;
}
EXPORT_SYMBOL(page_pool_get_stats);

u8 *page_pool_ethtool_stats_get_strings(u8 *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pp_stats); i++) {
		memcpy(data, pp_stats[i], ETH_GSTRING_LEN);
		data += ETH_GSTRING_LEN;
	}

	return data;
}
EXPORT_SYMBOL(page_pool_ethtool_stats_get_strings);

int page_pool_ethtool_stats_get_count(void)
{
	return ARRAY_SIZE(pp_stats);
}
EXPORT_SYMBOL(page_pool_ethtool_stats_get_count);

u64 *page_pool_ethtool_stats_get(u64 *data, const void *stats)
{
	const struct page_pool_stats *pool_stats = stats;

	*data++ = pool_stats->alloc_stats.fast;
	*data++ = pool_stats->alloc_stats.slow;
	*data++ = pool_stats->alloc_stats.slow_high_order;
	*data++ = pool_stats->alloc_stats.empty;
	*data++ = pool_stats->alloc_stats.refill;
	*data++ = pool_stats->alloc_stats.waive;
	*data++ = pool_stats->recycle_stats.cached;
	*data++ = pool_stats->recycle_stats.cache_full;
	*data++ = pool_stats->recycle_stats.ring;
	*data++ = pool_stats->recycle_stats.ring_full;
	*data++ = pool_stats->recycle_stats.released_refcnt;

	return data;
}
EXPORT_SYMBOL(page_pool_ethtool_stats_get);

#else
#define alloc_stat_inc(pool, __stat)
#define recycle_stat_inc(pool, __stat)
#define recycle_stat_add(pool, __stat, val)
#endif

#ifdef CONFIG_NET_CACHEFLOW
static struct proc_dir_entry *page_pool_root_dir;

enum {
	PAGE_POOL_ALLOC,
	PAGE_POOL_RING,
	PAGE_POOL_ARRAY,
	PAGE_POOL_UNALLOC,
};

static inline int page_pool_account_usage(struct page_pool *pool, netmem_ref netmem, int old_state, int new_state) {
	if (!pool->usage_track)
		return 0;

	if (old_state == PAGE_POOL_ALLOC) {
#if IS_ENABLED(CONFIG_NET_CACHEFLOW_DEBUG)
		if (unlikely(pool->allocated_pages == 0)) {
			pr_err("page_pool: allocated_pages is 0\n");
		}
#endif
		pool->allocated_pages--;
	} else if (old_state == PAGE_POOL_ARRAY) {
		if (unlikely(pool->array_pages == 0)) {
			pr_err("page_pool: array_pages is 0\n");
		}
		pool->array_pages--;
	} else if (old_state == PAGE_POOL_RING) {
		if (unlikely(pool->ring_pages == 0)) {
			pr_err("page_pool: ring_pages is 0\n");
		}
		pool->ring_pages--;
	}

	if (new_state == PAGE_POOL_ALLOC) {
		pool->allocated_pages++;
	} else if (new_state == PAGE_POOL_ARRAY) {
		pool->array_pages++;
	} else if (new_state == PAGE_POOL_RING) {
		pool->ring_pages++;
	}

	trace_page_pool_page_move(pool, netmem, old_state, new_state, 
			pool->allocated_pages,
			pool->array_pages,
			pool->ring_pages);

	return 0;
}

static int pool_stats_show(struct seq_file *m, void *v) {
	struct page_pool_proc *pool_proc = m->private;
	struct page_pool *pool = pool_proc->pool;
	struct page_pool_stats stats = { 0 };

	if (!page_pool_get_stats(pool, &stats))
		return -ENOMEM;

	seq_printf(m, "Pool: %p, ", pool);
	seq_printf(m, "Pool Name: %s, ", pool_proc->page_pool_name);
	seq_printf(m, "Fast: %llu, ", stats.alloc_stats.fast);
	seq_printf(m, "Slow: %llu, ", stats.alloc_stats.slow);
	seq_printf(m, "Slow High Order: %llu, ", stats.alloc_stats.slow_high_order);
	seq_printf(m, "Empty: %llu, ", stats.alloc_stats.empty);
	seq_printf(m, "Refill: %llu, ", stats.alloc_stats.refill);
	seq_printf(m, "Waive: %llu, ", stats.alloc_stats.waive);
	seq_printf(m, "Cached: %llu, ", stats.recycle_stats.cached);
	seq_printf(m, "Cache Full: %llu, ", stats.recycle_stats.cache_full);
	seq_printf(m, "Ring: %llu, ", stats.recycle_stats.ring);
	seq_printf(m, "Ring Full: %llu, ", stats.recycle_stats.ring_full);
	seq_printf(m, "Released Refcnt: %llu\n", stats.recycle_stats.released_refcnt);

	return 0;
}

static int pool_watermark_show(struct seq_file *m, void *v) {
	struct page_pool_proc *pool_proc = m->private;
	struct page_pool *pool = pool_proc->pool;

	seq_printf(m, "Pool Name: %s, ", pool_proc->page_pool_name);
	seq_printf(m, "Pool Size: %u, ", pool->array_pages + pool->ring_pages + pool->allocated_pages);
	seq_printf(m, "Free Pages: %u, ", pool->array_pages + pool->ring_pages);
	seq_printf(m, "Used Pages: %u\n", pool->allocated_pages);

	return 0;
}

static int pool_stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, pool_stats_show, pde_data(inode));
}

static int pool_water_open(struct inode *inode, struct file *file)
{
    return single_open(file, pool_watermark_show, pde_data(inode));
}

static const struct proc_ops pool_stats_ops = {
    .proc_open = pool_stats_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static const struct proc_ops watermark_stats_ops = {
    .proc_open = pool_water_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static struct page_pool_proc* create_proc_entry(struct page_pool *pool) {
	struct page_pool_proc *pool_proc;
	char page_pool_name[PAGE_POOL_NAME_MAX_LEN];

	if (!pool)
		return NULL;

	pool_proc = kzalloc(sizeof(*pool_proc), GFP_KERNEL);
    	if (!pool_proc)
        	return NULL;
	
	snprintf(page_pool_name, PAGE_POOL_NAME_MAX_LEN, "pagepool_%s_%d", dev_name(pool->p.dev), pool->p.napi->napi_id);
	pool_proc->pool = pool;
	strncpy(pool_proc->page_pool_name, page_pool_name, PAGE_POOL_NAME_MAX_LEN);
	pool_proc->proc_dir = proc_mkdir(page_pool_name, page_pool_root_dir);

	if (!pool_proc->proc_dir) {
		goto err_free;
	}

	pool_proc->stats_file = proc_create_data("stats", 0444, pool_proc->proc_dir,
						&pool_stats_ops, pool_proc);
	if (!pool_proc->stats_file) {
		goto err_remove_dir;
	}

	pool_proc->watermark_file = proc_create_data("watermark", 0444, pool_proc->proc_dir,
						&watermark_stats_ops, pool_proc);

	if (!pool_proc->watermark_file) {
		goto err_remove_stats;
	}
	return pool_proc;

err_remove_stats:
	proc_remove(pool_proc->stats_file);
err_remove_dir:
    	proc_remove(pool_proc->proc_dir);
err_free:
    	kfree(pool_proc);
    	return NULL;
}

static void free_proc_entry(struct page_pool *pool) {
	struct page_pool_proc* proc = pool->proc;
	if (!proc)
		return;

    	if (proc->stats_file)
        	proc_remove(proc->stats_file);
	if (proc->watermark_file)
		proc_remove(proc->watermark_file);
    	if (proc->proc_dir)
        	proc_remove(proc->proc_dir);
    
    	kfree(proc);
	pool->proc = NULL;
}
#else
static inline int page_pool_account_usage(struct page_pool *pool, netmem_ref netmem,int old_state, int new_state) {}
#endif

static void page_pool_return_page(struct page_pool *pool, netmem_ref netmem);

#ifdef CONFIG_PAGE_POOL_BULK
static struct kmem_cache *netmem_mini_array_cache;

static bool page_pool_pop_mini_array(struct page_pool *pool) {
	if (unlikely(pool->alloc.cache || pool->alloc.count)) { 
		BUG();
	}

	if (likely(pool->alloc.bulk_count)) {
		pool->alloc.cache = pool->alloc.bulk[--pool->alloc.bulk_count];
		pool->alloc.count = PP_ALLOC_CACHE_BULK;
		return true;
	}

	return false;
}

static void page_pool_recycle_mini_array(struct page_pool *pool) {
	int i, j, ret;

	if (unlikely(pool->alloc.bulk_count != PP_ALLOC_CACHE_BULK_SIZE || pool->alloc.count != PP_ALLOC_CACHE_BULK)) {
		BUG();
	}

	for (i = 0; i < pool->alloc.bulk_count / 2; i++) {
		if (in_softirq()) {
#ifdef CONFIG_PAGE_POOL_STACK
			ret = ptr_stack_push(&pool->stack, (__force void *)pool->alloc.bulk[i]);
#else
			ret = ptr_ring_produce(&pool->ring, (__force void *)pool->alloc.bulk[i]);
#endif
		} else {
#ifdef CONFIG_PAGE_POOL_STACK
			ret = ptr_stack_push_bh(&pool->stack, (__force void *)pool->alloc.bulk[i]);
#else
			ret = ptr_ring_produce_bh(&pool->ring, (__force void *)pool->alloc.bulk[i]);
#endif
		}

		if (unlikely(ret)) {
			for (j = 0; j < PP_ALLOC_CACHE_BULK; j++) {
				recycle_stat_inc(pool, ring_full);
				page_pool_account_usage(pool, pool->alloc.bulk[i][j], PAGE_POOL_ARRAY, PAGE_POOL_UNALLOC);
				page_pool_return_page(pool, pool->alloc.bulk[i][j]);
			}
			kmem_cache_free(netmem_mini_array_cache, pool->alloc.bulk[i]);
			pool->alloc.bulk[i] = 0;
		} else {
			for (j = 0; j < PP_ALLOC_CACHE_BULK; j++) {
				page_pool_account_usage(pool, pool->alloc.bulk[i][j], PAGE_POOL_ARRAY, PAGE_POOL_RING);
			}
		}
	}

	for (i = pool->alloc.bulk_count / 2; i < pool->alloc.bulk_count; i++) {
		pool->alloc.bulk[i - pool->alloc.bulk_count / 2] = pool->alloc.bulk[i];
	}
	pool->alloc.bulk_count = pool->alloc.bulk_count - pool->alloc.bulk_count / 2;
}

static bool page_pool_push_mini_array(struct page_pool *pool) {
	if (unlikely(!pool->alloc.cache || (pool->alloc.count != PP_ALLOC_CACHE_BULK))) { 
		BUG();
	}

	if (unlikely(pool->alloc.bulk_count >= PP_ALLOC_CACHE_BULK)) {
		page_pool_recycle_mini_array(pool);
	}

	pool->alloc.bulk[pool->alloc.bulk_count++] = pool->alloc.cache;
	pool->alloc.cache = kmem_cache_alloc(netmem_mini_array_cache, GFP_ATOMIC|GFP_NOWAIT);
	pool->alloc.count = 0;
	return true;
}
#else
static bool page_pool_producer_lock(struct page_pool *pool)
	__acquires(&pool->ring.producer_lock)
{
	bool in_softirq = in_softirq();

	if (in_softirq)
#ifdef CONFIG_PAGE_POOL_STACK
		spin_lock(&pool->stack.lock);
#else
		spin_lock(&pool->ring.producer_lock);
#endif
	else
#ifdef CONFIG_PAGE_POOL_STACK
		spin_lock_bh(&pool->stack.lock);
#else
		spin_lock_bh(&pool->ring.producer_lock);
#endif

	return in_softirq;
}

static void page_pool_producer_unlock(struct page_pool *pool,
				      bool in_softirq)
	__releases(&pool->ring.producer_lock)
{
	if (in_softirq)
#ifdef CONFIG_PAGE_POOL_STACK
		spin_unlock(&pool->stack.lock);
#else
		spin_unlock(&pool->ring.producer_lock);
#endif
	else
#ifdef CONFIG_PAGE_POOL_STACK
		spin_unlock_bh(&pool->stack.lock);
#else
		spin_unlock_bh(&pool->ring.producer_lock);
#endif
}
#endif

static void page_pool_struct_check(void)
{
	CACHELINE_ASSERT_GROUP_MEMBER(struct page_pool, frag, frag_users);
	CACHELINE_ASSERT_GROUP_MEMBER(struct page_pool, frag, frag_page);
	CACHELINE_ASSERT_GROUP_MEMBER(struct page_pool, frag, frag_offset);
	CACHELINE_ASSERT_GROUP_SIZE(struct page_pool, frag,
				    PAGE_POOL_FRAG_GROUP_ALIGN);
}

static int page_pool_init(struct page_pool *pool,
			  const struct page_pool_params *params,
			  int cpuid)
{
	unsigned int ring_qsize = 1024; /* Default */
	struct netdev_rx_queue *rxq;
	int err;

	page_pool_struct_check();

	memcpy(&pool->p, &params->fast, sizeof(pool->p));
	memcpy(&pool->slow, &params->slow, sizeof(pool->slow));

	pool->cpuid = cpuid;

	/* Validate only known flags were used */
	if (pool->slow.flags & ~PP_FLAG_ALL)
		return -EINVAL;

	if (pool->p.pool_size)
		ring_qsize = pool->p.pool_size;

	/* Sanity limit mem that can be pinned down */
	if (ring_qsize > 32768)
		return -E2BIG;

	/* DMA direction is either DMA_FROM_DEVICE or DMA_BIDIRECTIONAL.
	 * DMA_BIDIRECTIONAL is for allowing page used for DMA sending,
	 * which is the XDP_TX use-case.
	 */
	if (pool->slow.flags & PP_FLAG_DMA_MAP) {
		if ((pool->p.dma_dir != DMA_FROM_DEVICE) &&
		    (pool->p.dma_dir != DMA_BIDIRECTIONAL))
			return -EINVAL;

		pool->dma_map = true;
	}

	if (pool->slow.flags & PP_FLAG_DMA_SYNC_DEV) {
		/* In order to request DMA-sync-for-device the page
		 * needs to be mapped
		 */
		if (!(pool->slow.flags & PP_FLAG_DMA_MAP))
			return -EINVAL;

		if (!pool->p.max_len)
			return -EINVAL;

		pool->dma_sync = true;

		/* pool->p.offset has to be set according to the address
		 * offset used by the DMA engine to start copying rx data
		 */
	}

	pool->has_init_callback = !!pool->slow.init_callback;

#ifdef CONFIG_PAGE_POOL_STATS
	if (!(pool->slow.flags & PP_FLAG_SYSTEM_POOL)) {
		pool->recycle_stats = alloc_percpu(struct page_pool_recycle_stats);
		if (!pool->recycle_stats)
			return -ENOMEM;
	} else {
		/* For system page pool instance we use a singular stats object
		 * instead of allocating a separate percpu variable for each
		 * (also percpu) page pool instance.
		 */
		pool->recycle_stats = &pp_system_recycle_stats;
		pool->system = true;
	}
#endif
#ifdef CONFIG_PAGE_POOL_STACK
	if (ptr_stack_init(&pool->stack, ring_qsize, GFP_KERNEL) < 0)
#else
	if (ptr_ring_init(&pool->ring, ring_qsize, GFP_KERNEL) < 0) 
#endif
	{
#ifdef CONFIG_PAGE_POOL_STATS
		if (!pool->system)
			free_percpu(pool->recycle_stats);
#endif
		return -ENOMEM;
	}


	atomic_set(&pool->pages_state_release_cnt, 0);

	/* Driver calling page_pool_create() also call page_pool_destroy() */
	refcount_set(&pool->user_cnt, 1);

	if (pool->dma_map)
		get_device(pool->p.dev);

	if (pool->slow.flags & PP_FLAG_ALLOW_UNREADABLE_NETMEM) {
		/* We rely on rtnl_lock()ing to make sure netdev_rx_queue
		 * configuration doesn't change while we're initializing
		 * the page_pool.
		 */
		ASSERT_RTNL();
		rxq = __netif_get_rx_queue(pool->slow.netdev,
					   pool->slow.queue_idx);
		pool->mp_priv = rxq->mp_params.mp_priv;
	}

	if (pool->mp_priv) {
		err = mp_dmabuf_devmem_init(pool);
		if (err) {
			pr_warn("%s() mem-provider init failed %d\n", __func__,
				err);
			goto free_ptr_ring;
		}

		static_branch_inc(&page_pool_mem_providers);
	}
#ifdef CONFIG_NET_CACHEFLOW
	if (pool->slow.flags & PP_FLAG_USAGE_TRACK) {
#ifdef CONFIG_PAGE_POOL_STACK
		u32 size = pool->stack.size;
#else
		u32 size = pool->ring.size;
#endif
		pool->usage_track = 1;

		pool->array_pages = 0;
		pool->ring_pages = 0;
		pool->allocated_pages = 0;
		pr_warn("page_pool: create fixed size pool with %u pages", size);

		pool->proc = create_proc_entry(pool);
		if (!pool->proc) {
			pr_err("page_pool: failed to create the proc entry\n");
			err = -ENOMEM;
			goto free_ptr_ring;
		}
	}

	if (pool->slow.flags & PP_FLAG_SINGLE_OWNER)
		pool->single_owner = 1;
#endif

	return 0;

free_ptr_ring:
#ifdef CONFIG_PAGE_POOL_STACK
	ptr_stack_cleanup(&pool->stack, NULL);
#else
	ptr_ring_cleanup(&pool->ring, NULL);
#endif
#ifdef CONFIG_PAGE_POOL_STATS
	if (!pool->system)
		free_percpu(pool->recycle_stats);
#endif
	return err;
}

static void page_pool_uninit(struct page_pool *pool)
{
#ifdef CONFIG_PAGE_POOL_STACK
	ptr_stack_cleanup(&pool->stack, NULL);
#else
	ptr_ring_cleanup(&pool->ring, NULL);
#endif

	if (pool->dma_map)
		put_device(pool->p.dev);

#ifdef CONFIG_PAGE_POOL_STATS
	if (!pool->system)
		free_percpu(pool->recycle_stats);
#endif
}

/**
 * page_pool_create_percpu() - create a page pool for a given cpu.
 * @params: parameters, see struct page_pool_params
 * @cpuid: cpu identifier
 */
struct page_pool *
page_pool_create_percpu(const struct page_pool_params *params, int cpuid)
{
	struct page_pool *pool;
	int err;

	pool = kzalloc_node(sizeof(*pool), GFP_KERNEL, params->nid);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	err = page_pool_init(pool, params, cpuid);
	if (err < 0)
		goto err_free;

	err = page_pool_list(pool);
	if (err)
		goto err_uninit;

	return pool;

err_uninit:
	page_pool_uninit(pool);
err_free:
	pr_warn("%s() gave up with errno %d\n", __func__, err);
	kfree(pool);
	return ERR_PTR(err);
}
EXPORT_SYMBOL(page_pool_create_percpu);

/**
 * page_pool_create() - create a page pool
 * @params: parameters, see struct page_pool_params
 */
struct page_pool *page_pool_create(const struct page_pool_params *params)
{
	return page_pool_create_percpu(params, -1);
}
EXPORT_SYMBOL(page_pool_create);

static noinline netmem_ref page_pool_refill_alloc_cache(struct page_pool *pool)
{
#ifdef CONFIG_PAGE_POOL_STACK
	struct ptr_stack *r = &pool->stack;
#else
	struct ptr_ring *r = &pool->ring;
#endif
	netmem_ref netmem = 0;
#ifdef CONFIG_PAGE_POOL_BULK
	netmem_mini_array_t bulk;
	int i;
#endif
	int pref_nid; /* preferred NUMA node */

	/* Quicker fallback, avoid locks when ring is empty */
#ifdef CONFIG_PAGE_POOL_STACK
	if (ptr_stack_empty(r))
#else
	if (__ptr_ring_empty(r)) 
#endif
	{
		alloc_stat_inc(pool, empty);
		return 0;
	}

	/* Softirq guarantee CPU and thus NUMA node is stable. This,
	 * assumes CPU refilling driver RX-ring will also run RX-NAPI.
	 */
#ifdef CONFIG_NUMA
	pref_nid = (pool->p.nid == NUMA_NO_NODE) ? numa_mem_id() : pool->p.nid;
#else
	/* Ignore pool->p.nid setting if !CONFIG_NUMA, helps compiler */
	pref_nid = numa_mem_id(); /* will be zero like page_to_nid() */
#endif

#ifdef CONFIG_PAGE_POOL_BULK
	do {

#ifdef CONFIG_PAGE_POOL_STACK
		bulk = (netmem_mini_array_t)ptr_stack_pop(r);
#else
		bulk = (netmem_mini_array_t)__ptr_ring_consume(r);
#endif

		if (unlikely(!bulk))
			break;
		
		// TODO(cacheflow): validate netmem numa node in batch.
		for (i = 0; i < PP_ALLOC_CACHE_BULK; i++) {
			page_pool_account_usage(pool, bulk[i], PAGE_POOL_RING, PAGE_POOL_ARRAY);
		}

		pool->alloc.bulk[pool->alloc.bulk_count++] = bulk;

	} while (pool->alloc.bulk_count < PP_ALLOC_CACHE_BULK_REFILL);

	page_pool_pop_mini_array(pool);
#else
	/* Refill alloc array, but only if NUMA match */
	do {
#ifdef CONFIG_PAGE_POOL_STACK
		netmem = (__force netmem_ref)ptr_stack_pop(r);
#else
		netmem = (__force netmem_ref)__ptr_ring_consume(r);
#endif
		if (unlikely(!netmem))
			break;

		if (likely(netmem_is_pref_nid(netmem, pref_nid))) {
			page_pool_account_usage(pool, netmem, PAGE_POOL_RING, PAGE_POOL_ARRAY);
			pool->alloc.cache[pool->alloc.count++] = netmem;
		} else {
			/* NUMA mismatch;
			 * (1) release 1 page to page-allocator and
			 * (2) break out to fallthrough to alloc_pages_node.
			 * This limit stress on page buddy alloactor.
			 */
			page_pool_account_usage(pool, netmem, PAGE_POOL_RING, PAGE_POOL_UNALLOC);


			page_pool_return_page(pool, netmem);
			alloc_stat_inc(pool, waive);

			netmem = 0;
			break;
		}
	} while (pool->alloc.count < PP_ALLOC_CACHE_REFILL);
#endif

	/* Return last page */
	if (likely(pool->alloc.count > 0)) {
		netmem = pool->alloc.cache[--pool->alloc.count];

		page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);

		alloc_stat_inc(pool, refill);
	}

	return netmem;
}

/* fast path */
#ifdef CONFIG_PAGE_POOL_BULK
static netmem_ref __page_pool_get_cached(struct page_pool *pool)
{
	netmem_ref netmem;

recheck:
	/* Caller MUST guarantee safe non-concurrent access, e.g. softirq */
	if (likely(pool->alloc.count)) {
		/* Fast-path */
		netmem = pool->alloc.cache[--pool->alloc.count];
		page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);
		alloc_stat_inc(pool, fast);

		return netmem;
	}
	
	if (likely(pool->alloc.cache)) {
		kmem_cache_free(netmem_mini_array_cache, pool->alloc.cache);
		pool->alloc.cache = 0;
	}

	if (likely(pool->alloc.bulk_count)) {
		page_pool_pop_mini_array(pool);
		goto recheck;
	} else {
		netmem = page_pool_refill_alloc_cache(pool);
	}

	return netmem;
}
#else
static netmem_ref __page_pool_get_cached(struct page_pool *pool)
{
	netmem_ref netmem;

	/* Caller MUST guarantee safe non-concurrent access, e.g. softirq */
	if (likely(pool->alloc.count)) {
		/* Fast-path */
		netmem = pool->alloc.cache[--pool->alloc.count];

		page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);

		alloc_stat_inc(pool, fast);
	} else {
		netmem = page_pool_refill_alloc_cache(pool);
	}
	return netmem;
}
#endif

static void __page_pool_dma_sync_for_device(const struct page_pool *pool,
					    netmem_ref netmem,
					    u32 dma_sync_size)
{
#if defined(CONFIG_HAS_DMA) && defined(CONFIG_DMA_NEED_SYNC)
	dma_addr_t dma_addr = page_pool_get_dma_addr_netmem(netmem);

	dma_sync_size = min(dma_sync_size, pool->p.max_len);
	__dma_sync_single_for_device(pool->p.dev, dma_addr + pool->p.offset,
				     dma_sync_size, pool->p.dma_dir);
#endif
}

static __always_inline void
page_pool_dma_sync_for_device(const struct page_pool *pool,
			      netmem_ref netmem,
			      u32 dma_sync_size)
{
	if (pool->dma_sync && dma_dev_need_sync(pool->p.dev))
		__page_pool_dma_sync_for_device(pool, netmem, dma_sync_size);
}

static bool page_pool_dma_map(struct page_pool *pool, netmem_ref netmem)
{
	dma_addr_t dma;

	/* Setup DMA mapping: use 'struct page' area for storing DMA-addr
	 * since dma_addr_t can be either 32 or 64 bits and does not always fit
	 * into page private data (i.e 32bit cpu with 64bit DMA caps)
	 * This mapping is kept for lifetime of page, until leaving pool.
	 */
	dma = dma_map_page_attrs(pool->p.dev, netmem_to_page(netmem), 0,
				 (PAGE_SIZE << pool->p.order), pool->p.dma_dir,
				 DMA_ATTR_SKIP_CPU_SYNC |
					 DMA_ATTR_WEAK_ORDERING);
	if (dma_mapping_error(pool->p.dev, dma))
		return false;

	if (page_pool_set_dma_addr_netmem(netmem, dma))
		goto unmap_failed;

	page_pool_dma_sync_for_device(pool, netmem, pool->p.max_len);

	return true;

unmap_failed:
	WARN_ONCE(1, "unexpected DMA address, please report to netdev@");
	dma_unmap_page_attrs(pool->p.dev, dma,
			     PAGE_SIZE << pool->p.order, pool->p.dma_dir,
			     DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING);
	return false;
}

static struct page *__page_pool_alloc_page_order(struct page_pool *pool,
						 gfp_t gfp)
{
	struct page *page;

	gfp |= __GFP_COMP;
	page = alloc_pages_node(pool->p.nid, gfp, pool->p.order);
	if (unlikely(!page))
		return NULL;

	if (pool->dma_map && unlikely(!page_pool_dma_map(pool, page_to_netmem(page)))) {
		put_page(page);
		return NULL;
	}

	alloc_stat_inc(pool, slow_high_order);
	page_pool_set_pp_info(pool, page_to_netmem(page));

	/* Track how many pages are held 'in-flight' */
	pool->pages_state_hold_cnt++;
	trace_page_pool_state_hold(pool, page_to_netmem(page),
				   pool->pages_state_hold_cnt);
	return page;
}

/* slow path */
static noinline netmem_ref __page_pool_alloc_pages_slow(struct page_pool *pool,
							gfp_t gfp)
{
	const int bulk = PP_ALLOC_CACHE_REFILL;
	unsigned int pp_order = pool->p.order;
	bool dma_map = pool->dma_map;
	netmem_ref netmem;
	int i, j, nr_pages;

	/* Don't support bulk alloc for high-order pages */
	if (unlikely(pp_order)) {
		netmem = page_to_netmem(__page_pool_alloc_page_order(pool, gfp));
		page_pool_account_usage(pool, netmem, PAGE_POOL_UNALLOC, PAGE_POOL_ALLOC);
		return netmem;
	}

	/* Unnecessary as alloc cache is empty, but guarantees zero count */
	if (unlikely(pool->alloc.count > 0)) {
		netmem = pool->alloc.cache[--pool->alloc.count];

		page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);
		return netmem;
	}

#ifdef CONFIG_PAGE_POOL_BULK
	for (i = 0; i < bulk / PP_ALLOC_CACHE_BULK; i++) {
		pool->alloc.cache = kmem_cache_alloc(netmem_mini_array_cache, GFP_ATOMIC|GFP_NOWAIT);
		if (unlikely(!pool->alloc.cache))
			goto out;

		pool->alloc.count = 0;

		memset(pool->alloc.cache, 0, sizeof(void *) * PP_ALLOC_CACHE_BULK);

		nr_pages = alloc_pages_bulk_array_node(gfp,
						pool->p.nid, PP_ALLOC_CACHE_BULK,
						(struct page **)pool->alloc.cache);

		if (unlikely(!nr_pages)) {
			kmem_cache_free(netmem_mini_array_cache, pool->alloc.cache);
			pool->alloc.cache = 0;
			goto out;
		}

		/* Pages have been filled into alloc.cache array, but count is zero and
		* page element have not been (possibly) DMA mapped.
		*/
		for (j = 0; j < nr_pages; j++) {
			netmem = pool->alloc.cache[j];
			if (dma_map && unlikely(!page_pool_dma_map(pool, netmem))) {
				put_page(netmem_to_page(netmem));
				continue;
			}

			page_pool_set_pp_info(pool, netmem);
			pool->alloc.cache[pool->alloc.count++] = netmem;

			/* Track how many pages are held 'in-flight' */
			pool->pages_state_hold_cnt++;
			trace_page_pool_state_hold(pool, netmem,
						pool->pages_state_hold_cnt);
			page_pool_account_usage(pool, netmem, PAGE_POOL_UNALLOC, PAGE_POOL_ARRAY);
		}

		if (likely(pool->alloc.count == PP_ALLOC_CACHE_BULK)) {
			pool->alloc.bulk[pool->alloc.bulk_count++] = pool->alloc.cache;
			pool->alloc.cache = 0;
			pool->alloc.count = 0;
		} else if (unlikely(pool->alloc.count == 0)) {
			kmem_cache_free(netmem_mini_array_cache, pool->alloc.cache);
			pool->alloc.cache = 0;
			break;
		} else {
			break;
		}
	}
out:
	if (pool->alloc.count == 0) {
		page_pool_pop_mini_array(pool);
	}
#else
	/* Mark empty alloc.cache slots "empty" for alloc_pages_bulk_array */
	memset(&pool->alloc.cache, 0, sizeof(void *) * bulk);

	nr_pages = alloc_pages_bulk_array_node(gfp,
					       pool->p.nid, bulk,
					       (struct page **)pool->alloc.cache);
	if (unlikely(!nr_pages))
		return 0;

	/* Pages have been filled into alloc.cache array, but count is zero and
	 * page element have not been (possibly) DMA mapped.
	 */
	for (i = 0; i < nr_pages; i++) {
		netmem = pool->alloc.cache[i];
		if (dma_map && unlikely(!page_pool_dma_map(pool, netmem))) {
			put_page(netmem_to_page(netmem));
			continue;
		}

		page_pool_set_pp_info(pool, netmem);
		pool->alloc.cache[pool->alloc.count++] = netmem;
		/* Track how many pages are held 'in-flight' */
		pool->pages_state_hold_cnt++;
		trace_page_pool_state_hold(pool, netmem,
					   pool->pages_state_hold_cnt);
		page_pool_account_usage(pool, netmem, PAGE_POOL_UNALLOC, PAGE_POOL_ARRAY);
	}
#endif

	/* Return last page */
	if (likely(pool->alloc.count > 0)) {
		netmem = pool->alloc.cache[--pool->alloc.count];

		page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);
		alloc_stat_inc(pool, slow);
	} else {
		netmem = 0;
	}

	/* When page just alloc'ed is should/must have refcnt 1. */
	return netmem;
}

/* For using page_pool replace: alloc_pages() API calls, but provide
 * synchronization guarantee for allocation side.
 */
netmem_ref page_pool_alloc_netmem(struct page_pool *pool, gfp_t gfp)
{
	netmem_ref netmem = 0;

	/* Fast-path: Get a page from cache */
	netmem = __page_pool_get_cached(pool);
	if (netmem)
		return netmem;

	/* Slow-path: cache empty, do real allocation */
	if (static_branch_unlikely(&page_pool_mem_providers) && pool->mp_priv)
		netmem = mp_dmabuf_devmem_alloc_netmems(pool, gfp);
	else {
		netmem = __page_pool_alloc_pages_slow(pool, gfp);
	}
	return netmem;
}
EXPORT_SYMBOL(page_pool_alloc_netmem);

struct page *page_pool_alloc_pages(struct page_pool *pool, gfp_t gfp)
{
	return netmem_to_page(page_pool_alloc_netmem(pool, gfp));
}
EXPORT_SYMBOL(page_pool_alloc_pages);
ALLOW_ERROR_INJECTION(page_pool_alloc_pages, NULL);

/* Calculate distance between two u32 values, valid if distance is below 2^(31)
 *  https://en.wikipedia.org/wiki/Serial_number_arithmetic#General_Solution
 */
#define _distance(a, b)	(s32)((a) - (b))

s32 page_pool_inflight(const struct page_pool *pool, bool strict)
{
	u32 release_cnt = atomic_read(&pool->pages_state_release_cnt);
	u32 hold_cnt = READ_ONCE(pool->pages_state_hold_cnt);
	s32 inflight;

	inflight = _distance(hold_cnt, release_cnt);

	if (strict) {
		trace_page_pool_release(pool, inflight, hold_cnt, release_cnt);
		WARN(inflight < 0, "Negative(%d) inflight packet-pages",
		     inflight);
	} else {
		inflight = max(0, inflight);
	}

	return inflight;
}

void page_pool_set_pp_info(struct page_pool *pool, netmem_ref netmem)
{
	netmem_set_pp(netmem, pool);
	netmem_or_pp_magic(netmem, PP_SIGNATURE);

	/* Ensuring all pages have been split into one fragment initially:
	 * page_pool_set_pp_info() is only called once for every page when it
	 * is allocated from the page allocator and page_pool_fragment_page()
	 * is dirtying the same cache line as the page->pp_magic above, so
	 * the overhead is negligible.
	 */
	page_pool_fragment_netmem(netmem, 1);
	if (pool->has_init_callback)
		pool->slow.init_callback(netmem, pool->slow.init_arg);
}

void page_pool_clear_pp_info(netmem_ref netmem)
{
	netmem_clear_pp_magic(netmem);
	netmem_set_pp(netmem, NULL);
}

static __always_inline void __page_pool_release_page_dma(struct page_pool *pool,
							 netmem_ref netmem)
{
	dma_addr_t dma;

	if (!pool->dma_map)
		/* Always account for inflight pages, even if we didn't
		 * map them
		 */
		return;

	dma = page_pool_get_dma_addr_netmem(netmem);

	/* When page is unmapped, it cannot be returned to our pool */
	dma_unmap_page_attrs(pool->p.dev, dma,
			     PAGE_SIZE << pool->p.order, pool->p.dma_dir,
			     DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING);
	page_pool_set_dma_addr_netmem(netmem, 0);
}

/* Disconnects a page (from a page_pool).  API users can have a need
 * to disconnect a page (from a page_pool), to allow it to be used as
 * a regular page (that will eventually be returned to the normal
 * page-allocator via put_page).
 */
static void __page_pool_return_page(struct page_pool *pool, netmem_ref netmem)
{
	int count;
	bool put;

	put = true;
	if (static_branch_unlikely(&page_pool_mem_providers) && pool->mp_priv)
		put = mp_dmabuf_devmem_release_page(pool, netmem);
	else
		__page_pool_release_page_dma(pool, netmem);

	/* This may be the last page returned, releasing the pool, so
	 * it is not safe to reference pool afterwards.
	 */
	count = atomic_inc_return_relaxed(&pool->pages_state_release_cnt);
	trace_page_pool_state_release(pool, netmem, count);

	if (put) {
		page_pool_clear_pp_info(netmem);
		put_page(netmem_to_page(netmem));
	}
	/* An optimization would be to call __free_pages(page, pool->p.order)
	 * knowing page is not part of page-cache (thus avoiding a
	 * __page_cache_release() call).
	 */
}

void page_pool_return_page(struct page_pool *pool, netmem_ref netmem)
{
	__page_pool_return_page(pool, netmem);
}

static void page_pool_release_return_page(struct page_pool *pool, netmem_ref netmem)
{
	__page_pool_return_page(pool, netmem);
}

#ifndef CONFIG_PAGE_POOL_BULK
static bool page_pool_recycle_in_ring(struct page_pool *pool, netmem_ref netmem)
{
	int ret;
	/* BH protection not needed if current is softirq */
	if (in_softirq())
#ifdef CONFIG_PAGE_POOL_STACK
		ret = ptr_stack_push(&pool->stack, (__force void *)netmem);
#else
		ret = ptr_ring_produce(&pool->ring, (__force void *)netmem);
#endif
	else
#ifdef CONFIG_PAGE_POOL_STACK
		ret = ptr_stack_push_bh(&pool->stack, (__force void *)netmem);
#else
		ret = ptr_ring_produce_bh(&pool->ring, (__force void *)netmem);
#endif

	if (!ret) {
		page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_RING);
		recycle_stat_inc(pool, ring);
		return true;
	}

	return false;
}
#endif

/* Only allow direct recycling in special circumstances, into the
 * alloc side cache.  E.g. during RX-NAPI processing for XDP_DROP use-case.
 *
 * Caller must provide appropriate safe context.
 */
static bool page_pool_recycle_in_cache(netmem_ref netmem,
				       struct page_pool *pool)
{
#ifdef CONFIG_PAGE_POOL_BULK
	if (pool->alloc.count == PP_ALLOC_CACHE_BULK) {
		page_pool_push_mini_array(pool);
	}
#else
	if (unlikely(pool->alloc.count == PP_ALLOC_CACHE_SIZE)) {
		recycle_stat_inc(pool, cache_full);
		return false;
	}
#endif

	/* Caller MUST have verified/know (page_ref_count(page) == 1) */
	pool->alloc.cache[pool->alloc.count++] = netmem;

	page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_ARRAY);
	recycle_stat_inc(pool, cached);
	return true;
}

static bool __page_pool_page_can_be_recycled(netmem_ref netmem)
{
	return netmem_is_net_iov(netmem) ||
	       (page_ref_count(netmem_to_page(netmem)) == 1 &&
		!page_is_pfmemalloc(netmem_to_page(netmem)));
}

/* If the page refcnt == 1, this will try to recycle the page.
 * If pool->dma_sync is set, we'll try to sync the DMA area for
 * the configured size min(dma_sync_size, pool->max_len).
 * If the page refcnt != 1, then the page will be returned to memory
 * subsystem.
 */
static __always_inline netmem_ref
__page_pool_put_page(struct page_pool *pool, netmem_ref netmem,
		     unsigned int dma_sync_size, bool allow_direct)
{
	lockdep_assert_no_hardirq();

	/* This allocator is optimized for the XDP mode that uses
	 * one-frame-per-page, but have fallbacks that act like the
	 * regular page allocator APIs.
	 *
	 * refcnt == 1 means page_pool owns page, and can recycle it.
	 *
	 * page is NOT reusable when allocated when system is under
	 * some pressure. (page_is_pfmemalloc)
	 */
	if (likely(__page_pool_page_can_be_recycled(netmem))) {
		/* Read barrier done in page_ref_count / READ_ONCE */

		page_pool_dma_sync_for_device(pool, netmem, dma_sync_size);

		if (allow_direct && page_pool_recycle_in_cache(netmem, pool)) {
			return 0;
		}

		/* Page found as candidate for recycling */
		return netmem;
	}

	/* Fallback/non-XDP mode: API user have elevated refcnt.
	 *
	 * Many drivers split up the page into fragments, and some
	 * want to keep doing this to save memory and do refcnt based
	 * recycling. Support this use case too, to ease drivers
	 * switching between XDP/non-XDP.
	 *
	 * In-case page_pool maintains the DMA mapping, API user must
	 * call page_pool_put_page once.  In this elevated refcnt
	 * case, the DMA is unmapped/released, as driver is likely
	 * doing refcnt based recycle tricks, meaning another process
	 * will be invoking put_page.
	 */
	page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
	recycle_stat_inc(pool, released_refcnt);
	page_pool_return_page(pool, netmem);

	return 0;
}

static bool page_pool_napi_local(const struct page_pool *pool)
{
	const struct napi_struct *napi;
	u32 cpuid;

	if (unlikely(!in_softirq())) {
		trace_page_pool_napi_local_check(pool, 0, -1, -1, -1);
		return false;
	}

	/* Allow direct recycle if we have reasons to believe that we are
	 * in the same context as the consumer would run, so there's
	 * no possible race.
	 * __page_pool_put_page() makes sure we're not in hardirq context
	 * and interrupts are enabled prior to accessing the cache.
	 */
	cpuid = smp_processor_id();
	if (READ_ONCE(pool->cpuid) == cpuid) {
		trace_page_pool_napi_local_check(pool, 1, cpuid, READ_ONCE(pool->cpuid), -1);
		return true;
	}

	napi = READ_ONCE(pool->p.napi);

	if (napi && READ_ONCE(napi->list_owner) == cpuid) {
		trace_page_pool_napi_local_check(pool, 2, cpuid, READ_ONCE(pool->cpuid), READ_ONCE(napi->list_owner));
		return true;
	}

	trace_page_pool_napi_local_check(pool, 3, cpuid, READ_ONCE(pool->cpuid), napi ? READ_ONCE(napi->list_owner) : -1);
	return false;
}

void page_pool_put_unrefed_netmem(struct page_pool *pool, netmem_ref netmem,
				  unsigned int dma_sync_size, bool allow_direct)
{
	if (!allow_direct)
		allow_direct = page_pool_napi_local(pool);

	netmem =
		__page_pool_put_page(pool, netmem, dma_sync_size, allow_direct);

#ifdef CONFIG_PAGE_POOL_BULK
	if (netmem) {
		recycle_stat_inc(pool, ring_full);
		page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
		page_pool_return_page(pool, netmem);
	}
#else
	if (netmem && !page_pool_recycle_in_ring(pool, netmem)) {
		/* Cache full, fallback to free pages */
		recycle_stat_inc(pool, ring_full);
		page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
		page_pool_return_page(pool, netmem);
	}
#endif
}
EXPORT_SYMBOL(page_pool_put_unrefed_netmem);

void page_pool_put_unrefed_page(struct page_pool *pool, struct page *page,
				unsigned int dma_sync_size, bool allow_direct)
{
	page_pool_put_unrefed_netmem(pool, page_to_netmem(page), dma_sync_size,
				     allow_direct);
}
EXPORT_SYMBOL(page_pool_put_unrefed_page);

/**
 * page_pool_put_page_bulk() - release references on multiple pages
 * @pool:	pool from which pages were allocated
 * @data:	array holding page pointers
 * @count:	number of pages in @data
 *
 * Tries to refill a number of pages into the ptr_ring cache holding ptr_ring
 * producer lock. If the ptr_ring is full, page_pool_put_page_bulk()
 * will release leftover pages to the page allocator.
 * page_pool_put_page_bulk() is suitable to be run inside the driver NAPI tx
 * completion loop for the XDP_REDIRECT use case.
 *
 * Please note the caller must not use data area after running
 * page_pool_put_page_bulk(), as this function overwrites it.
 */
void page_pool_put_page_bulk(struct page_pool *pool, void **data,
			     int count)
{
	int i = 0, bulk_len = 0;
	bool allow_direct;

#ifndef CONFIG_PAGE_POOL_BULK
	bool in_softirq;
#endif

	allow_direct = page_pool_napi_local(pool);

	for (i = 0; i < count; i++) {
		netmem_ref netmem = page_to_netmem(virt_to_head_page(data[i]));

		/* It is not the last user for the page frag case */
		if (!page_pool_is_last_ref(netmem))
			continue;

		netmem = __page_pool_put_page(pool, netmem, -1, allow_direct);
		/* Approved for bulk recycling in ptr_ring cache */
		if (netmem)
			data[bulk_len++] = (__force void *)netmem;
	}

	if (!bulk_len)
		return;

#ifndef CONFIG_PAGE_POOL_BULK
	/* Bulk producer into ptr_ring page_pool cache */
	in_softirq = page_pool_producer_lock(pool);
	for (i = 0; i < bulk_len; i++) {
#ifdef CONFIG_PAGE_POOL_STACK
		if (__ptr_stack_push(&pool->stack, data[i]))
#else
		if (__ptr_ring_produce(&pool->ring, data[i])) 
#endif
		{
			/* ring full */
			recycle_stat_inc(pool, ring_full);
			break;
		}
		page_pool_account_usage(pool, (netmem_ref) data[i], PAGE_POOL_ALLOC, PAGE_POOL_RING);
	}
	recycle_stat_add(pool, ring, i);
	page_pool_producer_unlock(pool, in_softirq);

	/* Hopefully all pages was return into ptr_ring */
	if (likely(i == bulk_len))
		return;
#endif
	
	/* ptr_ring cache full, free remaining pages outside producer lock
	 * since put_page() with refcnt == 1 can be an expensive operation
	 */
	for (; i < bulk_len; i++) {
		page_pool_account_usage(pool, (__force netmem_ref)data[i], PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
		page_pool_return_page(pool, (__force netmem_ref)data[i]);
	}
}
EXPORT_SYMBOL(page_pool_put_page_bulk);

static netmem_ref page_pool_drain_frag(struct page_pool *pool,
				       netmem_ref netmem)
{
	long drain_count = BIAS_MAX - pool->frag_users;

	/* Some user is still using the page frag */
	if (likely(page_pool_unref_netmem(netmem, drain_count)))
		return 0;

	if (__page_pool_page_can_be_recycled(netmem)) {
		page_pool_dma_sync_for_device(pool, netmem, -1);
		return netmem;
	}

	page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
	page_pool_return_page(pool, netmem);
	return 0;
}

static void page_pool_free_frag(struct page_pool *pool)
{
	long drain_count = BIAS_MAX - pool->frag_users;
	netmem_ref netmem = pool->frag_page;

	pool->frag_page = 0;

	if (!netmem || page_pool_unref_netmem(netmem, drain_count))
		return;

	page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
	page_pool_return_page(pool, netmem);
}

netmem_ref page_pool_alloc_frag_netmem(struct page_pool *pool,
				       unsigned int *offset, unsigned int size,
				       gfp_t gfp)
{
	unsigned int max_size = PAGE_SIZE << pool->p.order;
	netmem_ref netmem = pool->frag_page;

	if (WARN_ON(size > max_size))
		return 0;

	size = ALIGN(size, dma_get_cache_alignment());
	*offset = pool->frag_offset;

	if (netmem && *offset + size > max_size) {
		netmem = page_pool_drain_frag(pool, netmem);
		if (netmem) {
			alloc_stat_inc(pool, fast);
			goto frag_reset;
		}
	}

	if (!netmem) {
		netmem = page_pool_alloc_netmem(pool, gfp);
		if (unlikely(!netmem)) {
			pool->frag_page = 0;
			return 0;
		}

		pool->frag_page = netmem;

frag_reset:
		pool->frag_users = 1;
		*offset = 0;
		pool->frag_offset = size;
		page_pool_fragment_netmem(netmem, BIAS_MAX);
		return netmem;
	}

	pool->frag_users++;
	pool->frag_offset = *offset + size;
	alloc_stat_inc(pool, fast);
	return netmem;
}
EXPORT_SYMBOL(page_pool_alloc_frag_netmem);

struct page *page_pool_alloc_frag(struct page_pool *pool, unsigned int *offset,
				  unsigned int size, gfp_t gfp)
{
	return netmem_to_page(page_pool_alloc_frag_netmem(pool, offset, size,
							  gfp));
}
EXPORT_SYMBOL(page_pool_alloc_frag);

#ifdef CONFIG_PAGE_POOL_BULK
static void page_pool_empty_ring(struct page_pool *pool)
{
	netmem_mini_array_t mini_array;
	int i;
#ifdef CONFIG_PAGE_POOL_STACK
	while ((mini_array = ptr_stack_pop_bh(&pool->stack))) 
#else
	while ((mini_array = ptr_ring_consume_bh(&pool->ring)))
#endif
	{
		for (i = 0; i < PP_ALLOC_CACHE_BULK; i++) {
			if (!(netmem_ref_count(mini_array[i]) == 1))
				pr_crit("%s() page_pool refcnt %d violation\n",
					__func__, netmem_ref_count(mini_array[i]));

			page_pool_account_usage(pool, (__force netmem_ref)mini_array[i], PAGE_POOL_RING, PAGE_POOL_UNALLOC);

			page_pool_return_page(pool, (__force netmem_ref)mini_array[i]);
		}
		kmem_cache_free(netmem_mini_array_cache, mini_array);
	}
}
#else
static void page_pool_empty_ring(struct page_pool *pool)
{
	netmem_ref netmem;

	/* Empty recycle ring */
#ifdef CONFIG_PAGE_POOL_STACK
	while ((netmem = (__force netmem_ref)ptr_stack_pop_bh(&pool->stack)))
#else
	while ((netmem = (__force netmem_ref)ptr_ring_consume_bh(&pool->ring))) 
#endif
	{
		/* Verify the refcnt invariant of cached pages */
		if (!(netmem_ref_count(netmem) == 1))
			pr_crit("%s() page_pool refcnt %d violation\n",
				__func__, netmem_ref_count(netmem));

		page_pool_account_usage(pool, netmem, PAGE_POOL_RING, PAGE_POOL_UNALLOC);

		page_pool_release_return_page(pool, netmem);
	}
}
#endif

static void __page_pool_destroy(struct page_pool *pool)
{
#ifdef CONFIG_NET_CACHEFLOW
	free_proc_entry(pool);
#endif
	if (pool->disconnect)
		pool->disconnect(pool);

	page_pool_unlist(pool);
	page_pool_uninit(pool);

	if (pool->mp_priv) {
		mp_dmabuf_devmem_destroy(pool);
		static_branch_dec(&page_pool_mem_providers);
	}

	kfree(pool);
}

#ifdef CONFIG_PAGE_POOL_BULK
static void page_pool_empty_mini_array(struct page_pool *pool, netmem_mini_array_t mini_array, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		page_pool_account_usage(pool, (__force netmem_ref)mini_array[i], PAGE_POOL_ARRAY, PAGE_POOL_UNALLOC);
		page_pool_release_return_page(pool, (__force netmem_ref)mini_array[i]);
	}
	kmem_cache_free(netmem_mini_array_cache, mini_array);
}
#endif

static void page_pool_empty_alloc_cache_once(struct page_pool *pool)
{
#ifndef CONFIG_PAGE_POOL_BULK
	netmem_ref netmem;
#endif

	if (pool->destroy_cnt)
		return;

	/* Empty alloc cache, assume caller made sure this is
	 * no-longer in use, and page_pool_alloc_pages() cannot be
	 * call concurrently.
	 */
#ifdef CONFIG_PAGE_POOL_BULK
	do {
		page_pool_empty_mini_array(pool, pool->alloc.cache, pool->alloc.count);
		pool->alloc.cache = 0;
		pool->alloc.count = 0;
	} while (page_pool_pop_mini_array(pool));
#else
	while (pool->alloc.count) {
		netmem = pool->alloc.cache[--pool->alloc.count];
		page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_UNALLOC);
		page_pool_release_return_page(pool, netmem);
	}
#endif

}

static void page_pool_scrub(struct page_pool *pool)
{
	page_pool_empty_alloc_cache_once(pool);
	pool->destroy_cnt++;

	/* No more consumers should exist, but producers could still
	 * be in-flight.
	 */
	page_pool_empty_ring(pool);
}

static int page_pool_release(struct page_pool *pool)
{
	int inflight;

	page_pool_scrub(pool);
	inflight = page_pool_inflight(pool, true);
	if (!inflight)
		__page_pool_destroy(pool);

	return inflight;
}

static void page_pool_release_retry(struct work_struct *wq)
{
	struct delayed_work *dwq = to_delayed_work(wq);
	struct page_pool *pool = container_of(dwq, typeof(*pool), release_dw);
	void *netdev;
	int inflight;

	inflight = page_pool_release(pool);
	if (!inflight)
		return;

	/* Periodic warning for page pools the user can't see */
	netdev = READ_ONCE(pool->slow.netdev);
	if (time_after_eq(jiffies, pool->defer_warn) &&
	    (!netdev || netdev == NET_PTR_POISON)) {
		int sec = (s32)((u32)jiffies - (u32)pool->defer_start) / HZ;

		pr_warn("%s() stalled pool shutdown: id %u, %d inflight %d sec\n",
			__func__, pool->user.id, inflight, sec);
		pool->defer_warn = jiffies + DEFER_WARN_INTERVAL;
	}

	/* Still not ready to be disconnected, retry later */
	schedule_delayed_work(&pool->release_dw, DEFER_TIME);
}

void page_pool_use_xdp_mem(struct page_pool *pool, void (*disconnect)(void *),
			   const struct xdp_mem_info *mem)
{
	refcount_inc(&pool->user_cnt);
	pool->disconnect = disconnect;
	pool->xdp_mem_id = mem->id;
}

void page_pool_disable_direct_recycling(struct page_pool *pool)
{
	/* Disable direct recycling based on pool->cpuid.
	 * Paired with READ_ONCE() in page_pool_napi_local().
	 */
	WRITE_ONCE(pool->cpuid, -1);

	if (!pool->p.napi)
		return;

	/* To avoid races with recycling and additional barriers make sure
	 * pool and NAPI are unlinked when NAPI is disabled.
	 */
	WARN_ON(!test_bit(NAPI_STATE_SCHED, &pool->p.napi->state));
	WARN_ON(READ_ONCE(pool->p.napi->list_owner) != -1);

	WRITE_ONCE(pool->p.napi, NULL);
}
EXPORT_SYMBOL(page_pool_disable_direct_recycling);

void page_pool_destroy(struct page_pool *pool)
{
	if (!pool)
		return;

	if (!page_pool_put(pool))
		return;

	page_pool_disable_direct_recycling(pool);
	page_pool_free_frag(pool);

	if (!page_pool_release(pool))
		return;

	page_pool_detached(pool);
	pool->defer_start = jiffies;
	pool->defer_warn  = jiffies + DEFER_WARN_INTERVAL;

	INIT_DELAYED_WORK(&pool->release_dw, page_pool_release_retry);
	schedule_delayed_work(&pool->release_dw, DEFER_TIME);
}
EXPORT_SYMBOL(page_pool_destroy);

/* Caller must provide appropriate safe context, e.g. NAPI. */
void page_pool_update_nid(struct page_pool *pool, int new_nid)
{
#ifndef CONFIG_PAGE_POOL_BULK
	netmem_ref netmem;
#endif
	trace_page_pool_update_nid(pool, new_nid);
	pool->p.nid = new_nid;

	/* Flush pool alloc cache, as refill will check NUMA node */
#ifdef CONFIG_PAGE_POOL_BULK
	do {
		page_pool_empty_mini_array(pool, pool->alloc.cache, pool->alloc.count);
		pool->alloc.cache = 0;
		pool->alloc.count = 0;
	} while (page_pool_pop_mini_array(pool));
#else
	while (pool->alloc.count) {
		netmem = pool->alloc.cache[--pool->alloc.count];
		page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_UNALLOC);
		page_pool_return_page(pool, netmem);
	}
#endif

}
EXPORT_SYMBOL(page_pool_update_nid);

#ifdef CONFIG_NET_CACHEFLOW
static int __init page_pool_proc_init(void)
{
    /* Create the parent directory /proc/pagepool/ */
    page_pool_root_dir = proc_mkdir("pagepool", NULL);
    if (!page_pool_root_dir)
        return -ENOMEM;

    pr_info("page pool: proc filesystem initialized\n");
    return 0;
}

module_init(page_pool_proc_init);

#ifdef CONFIG_PAGE_POOL_BULK
static int __init netmem_bulk_cache_init(void)
{
	netmem_mini_array_cache = kmem_cache_create("netmem_bulk_cache",
		sizeof(netmem_ref) * PP_ALLOC_CACHE_BULK, 0, 
		SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);
	return 0;
}

core_initcall(netmem_bulk_cache_init);
#endif

#endif
