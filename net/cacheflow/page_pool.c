/* SPDX-License-Identifier: GPL-2.0 
 *
 *	Author: Minhu Wang <minhuw@acm.org>
 * 	Modified from standard Linux page_pool.c
 */

#include <linux/error-injection.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>

#include <net/netdev_rx_queue.h>
#include <net/xdp.h>

#include <net/cacheflow/page_pool.h>
#include <net/cacheflow/cacheflow.h>

#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/page-flags.h>
#include <linux/mm.h> /* for put_page() */
#include <linux/poison.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>

#include <trace/events/page_pool.h>

#include "page_pool.h"


#define DEFER_TIME (msecs_to_jiffies(1000))
#define DEFER_WARN_INTERVAL (60 * HZ)

#define BIAS_MAX	(LONG_MAX >> 1)

static struct kmem_cache *netmem_mini_array_cache;

enum {
	PAGE_POOL_ALLOC,
	PAGE_POOL_RING,
	PAGE_POOL_ARRAY,
	PAGE_POOL_UNALLOC,
};

static inline int cacheflow_page_pool_account_usages(struct cacheflow_page_pool *pool, netmem_ref* netmem, int n, int old_state, int new_state) {
	if (unlikely(n == 0))
		return 0;

	if (old_state == PAGE_POOL_ALLOC) {
#if IS_ENABLED(CONFIG_NET_CACHEFLOW_DEBUG)
		if (unlikely(pool->allocated_pages == 0)) {
			pr_err("page_pool: alloc pages = %u, array_pages = %u, ring_pages = %u\n",
				pool->allocated_pages, pool->array_pages, pool->ring_pages);
			BUG();
		}
#endif
		pool->allocated_pages -= n;
	} else if (old_state == PAGE_POOL_ARRAY) {
#if IS_ENABLED(CONFIG_NET_CACHEFLOW_DEBUG)
		if (unlikely(pool->array_pages == 0)) {
			pr_err("page_pool: alloc pages = %u, array_pages = %u, ring_pages = %u\n",
				pool->allocated_pages, pool->array_pages, pool->ring_pages);
			BUG();
		}
#endif
		pool->array_pages -= n;
	} else if (old_state == PAGE_POOL_RING) {
#if IS_ENABLED(CONFIG_NET_CACHEFLOW_DEBUG)
		if (unlikely(pool->ring_pages == 0)) {
			pr_err("page_pool: alloc pages = %u, array_pages = %u, ring_pages = %u\n",
				pool->allocated_pages, pool->array_pages, pool->ring_pages);
			BUG();
		}
#endif
		pool->ring_pages -= n;
	}

	if (new_state == PAGE_POOL_ALLOC) {
		pool->allocated_pages += n;
	} else if (new_state == PAGE_POOL_ARRAY) {
		pool->array_pages += n;
	} else if (new_state == PAGE_POOL_RING) {
		pool->ring_pages += n;
	}

	if (tracepoint_enabled(page_pool_page_move)) {
		int i;
		for (i = 0; i < n; i++) {
			trace_cacheflow_page_pool_page_move(pool, netmem[i], old_state, new_state, 
				pool->allocated_pages,
				pool->array_pages,
				pool->ring_pages);
		}
	}

	return 0;
}

static inline int cacheflow_page_pool_account_usage(struct cacheflow_page_pool *pool, netmem_ref netmem, int old_state, int new_state) {
	return cacheflow_page_pool_account_usages(pool, &netmem, 1, old_state, new_state);
}

static void cacheflow_page_pool_return_page(struct cacheflow_page_pool *pool, netmem_ref netmem);

static inline void cacheflow_page_pool_put_empty_mini_array(struct cacheflow_page_pool *pool, struct netmem_mini_array* mini_array) {
	int i;
	if (!kasan_mempool_poison_object(mini_array))
		return;

	if (pool->alloc.empty_mini_array_count == PP_ALLOC_CACHE_BULK_FREE_CACHE_SIZE) {
		pool->alloc.empty_mini_array_count -= PP_ALLOC_CACHE_BULK_SIZE;
		for (i = 0; i < PP_ALLOC_CACHE_BULK_SIZE; i++) {
			kasan_mempool_unpoison_object(pool->alloc.empty_mini_array_cache[pool->alloc.empty_mini_array_count + i], kmem_cache_size(netmem_mini_array_cache));
		}
		kmem_cache_free_bulk(netmem_mini_array_cache, PP_ALLOC_CACHE_BULK_SIZE, 
					(void **) (pool->alloc.empty_mini_array_cache + pool->alloc.empty_mini_array_count));
	}
	pool->alloc.empty_mini_array_cache[pool->alloc.empty_mini_array_count++] = mini_array;
}

static inline struct netmem_mini_array* cacheflow_page_pool_get_mini_array(struct cacheflow_page_pool *pool) {
	int n;
	struct netmem_mini_array* mini_array;

	if (pool->alloc.empty_mini_array_count == 0) {
		n = kmem_cache_alloc_bulk(netmem_mini_array_cache, GFP_ATOMIC|GFP_NOWAIT, PP_ALLOC_CACHE_BULK_SIZE, 
					(void **) (pool->alloc.empty_mini_array_cache + pool->alloc.empty_mini_array_count));
		pool->alloc.empty_mini_array_count += n;
	}
	if (likely(pool->alloc.empty_mini_array_count > 0)) {
		mini_array = pool->alloc.empty_mini_array_cache[--pool->alloc.empty_mini_array_count];
		kasan_mempool_unpoison_object(mini_array, kmem_cache_size(netmem_mini_array_cache));
		return mini_array;
	}
	return NULL;
}

static inline bool cacheflow_page_pool_pop_mini_array(struct cacheflow_page_pool *pool) {
#ifdef CONFIG_NET_CACHEFLOW_DEBUG
	if (unlikely(pool->alloc.array || pool->alloc.array->count)) { 
		BUG();
	}
#endif

	if (likely(pool->alloc.full_mini_array_count)) {
		pool->alloc.array = pool->alloc.full_mini_array_cache[--pool->alloc.full_mini_array_count];
		return true;
	}

	return false;
}

static void cacheflow_page_pool_recycle_mini_array(struct cacheflow_page_pool *pool) {
	int i, j, ret;

#ifdef CONFIG_NET_CACHEFLOW_DEBUG
	if (unlikely(pool->alloc.full_mini_array_count != PP_ALLOC_CACHE_BULK_SIZE || pool->alloc.array->count != PP_ALLOC_CACHE_BULK)) {
		BUG();
	}
#endif

	for (i = 0; i < pool->alloc.full_mini_array_count / 2; i++) {
		ret = __ptr_stack_push(&pool->stack, (__force void *)pool->alloc.full_mini_array_cache[i]);

		if (likely(!ret)) {
			cacheflow_page_pool_account_usages(pool, pool->alloc.full_mini_array_cache[i]->array, pool->alloc.full_mini_array_cache[i]->count, PAGE_POOL_ARRAY, PAGE_POOL_RING);
		} else {
			cacheflow_page_pool_account_usages(pool, pool->alloc.full_mini_array_cache[i]->array, pool->alloc.full_mini_array_cache[i]->count, PAGE_POOL_ARRAY, PAGE_POOL_UNALLOC);

			for (j = 0; j < pool->alloc.full_mini_array_cache[i]->count; j++) {
				cacheflow_page_pool_return_page(pool, pool->alloc.full_mini_array_cache[i]->array[j]);
			}
			pool->alloc.full_mini_array_cache[i]->count = 0;

			cacheflow_page_pool_put_empty_mini_array(pool, pool->alloc.full_mini_array_cache[i]);
		}

		pool->alloc.full_mini_array_cache[i] = NULL;
	}

	for (i = pool->alloc.full_mini_array_count / 2; i < pool->alloc.full_mini_array_count; i++) {
		pool->alloc.full_mini_array_cache[i - pool->alloc.full_mini_array_count / 2] = pool->alloc.full_mini_array_cache[i];
	}
	pool->alloc.full_mini_array_count = pool->alloc.full_mini_array_count - pool->alloc.full_mini_array_count / 2;
}


static inline bool cacheflow_page_pool_push_mini_array(struct cacheflow_page_pool *pool) {
#ifdef CONFIG_NET_CACHEFLOW_DEBUG
	if (unlikely(!pool->alloc.array || (pool->alloc.array->count != PP_ALLOC_CACHE_BULK))) { 
		BUG();
	}
#endif

	if (unlikely(pool->alloc.full_mini_array_count >= PP_ALLOC_CACHE_BULK)) {
		cacheflow_page_pool_recycle_mini_array(pool);
	}

	pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count++] = pool->alloc.array;
	pool->alloc.array = cacheflow_page_pool_get_mini_array(pool);

#ifdef CONFIG_NET_CACHEFLOW_DEBUG
	if (unlikely(!pool->alloc.array || pool->alloc.array->count)) {
		BUG();
	}
#endif
	return true;
}

static int cacheflow_page_pool_init(struct cacheflow_page_pool *pool,
			  	const struct cacheflow_page_pool_params *params,
			  	int cpuid)
{
	unsigned int ring_qsize = 1024; /* Default */

	memcpy(&pool->p, &params->fast, sizeof(pool->p));
	memcpy(&pool->slow, &params->slow, sizeof(pool->slow));

	pool->cpuid = cpuid;

	if (pool->p.pool_size)
		ring_qsize = pool->p.pool_size;

	/* Sanity limit mem that can be pinned down */
	if (ring_qsize > 32768)
		return -E2BIG;

	/* DMA direction is either DMA_FROM_DEVICE or DMA_BIDIRECTIONAL.
	 * DMA_BIDIRECTIONAL is for allowing page used for DMA sending,
	 * which is the XDP_TX use-case.
	 */

	if ((pool->p.dma_dir != DMA_FROM_DEVICE) &&
		(pool->p.dma_dir != DMA_BIDIRECTIONAL))
		return -EINVAL;

	if (!pool->p.max_len)
		return -EINVAL;

#ifdef CONFIG_PAGE_POOL_STATS
	pool->recycle_stats = alloc_percpu(struct page_pool_recycle_stats);
	if (!pool->recycle_stats)
		return -ENOMEM;
#endif

	if (ptr_stack_init(&pool->stack, ring_qsize, GFP_KERNEL) < 0)
	{
#ifdef CONFIG_PAGE_POOL_STATS
		free_percpu(pool->recycle_stats);
#endif
		return -ENOMEM;
	}


	atomic_set(&pool->pages_state_release_cnt, 0);

	/* Driver calling page_pool_create() also call page_pool_destroy() */
	refcount_set(&pool->user_cnt, 1);

	get_device(pool->p.dev);

	pool->array_pages = 0;
	pool->ring_pages = 0;
	pool->allocated_pages = 0;

	ptr_ring_init(&pool->recycle_ring, 1024, GFP_KERNEL);

	return 0;
}

static void cacheflow_page_pool_uninit(struct cacheflow_page_pool *pool)
{
	ptr_stack_cleanup(&pool->stack, NULL);

	put_device(pool->p.dev);
}

/**
 * page_pool_create_percpu() - create a page pool for a given cpu.
 * @params: parameters, see struct page_pool_params
 * @cpuid: cpu identifier
 */
struct cacheflow_page_pool *
cacheflow_page_pool_create_percpu(const struct cacheflow_page_pool_params *params, int cpuid)
{
	struct cacheflow_page_pool *pool;
	int err;

	pool = kzalloc_node(sizeof(*pool), GFP_KERNEL, params->nid);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	err = cacheflow_page_pool_init(pool, params, cpuid);
	if (err < 0)
		goto err_free;

	return pool;

err_free:
	pr_warn("%s() gave up with errno %d\n", __func__, err);
	kfree(pool);
	return ERR_PTR(err);
}
EXPORT_SYMBOL(cacheflow_page_pool_create_percpu);

/**
 * page_pool_create() - create a page pool
 * @params: parameters, see struct page_pool_params
 */
struct cacheflow_page_pool *cacheflow_page_pool_create(const struct cacheflow_page_pool_params *params)
{
	return cacheflow_page_pool_create_percpu(params, -1);
}
EXPORT_SYMBOL(cacheflow_page_pool_create);

static noinline netmem_ref cacheflow_page_pool_refill_alloc_cache(struct cacheflow_page_pool *pool)
{
	struct ptr_stack *r = &pool->stack;
	struct netmem_mini_array *mini_array;
	netmem_ref netmem = 0;
	int pref_nid; /* preferred NUMA node */

	/* Quicker fallback, avoid locks when ring is empty */
	if (ptr_stack_empty(r)) {
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

	do {
		mini_array = (struct netmem_mini_array *)ptr_stack_pop(r);

		if (unlikely(!mini_array))
			break;
		
		cacheflow_page_pool_account_usages(pool, mini_array->array, mini_array->count, PAGE_POOL_RING, PAGE_POOL_ARRAY);

		pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count++] = mini_array;

	} while (pool->alloc.full_mini_array_count < PP_ALLOC_CACHE_BULK_REFILL);

	cacheflow_page_pool_pop_mini_array(pool);


	/* Return last page */
	if (likely(pool->alloc.array->count > 0)) {
		netmem = pool->alloc.array->array[--pool->alloc.array->count];

		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);
	}

	return netmem;
}

static netmem_ref __cacheflow_page_pool_get_cached(struct cacheflow_page_pool *pool)
{
	netmem_ref netmem;

recheck:
	/* Caller MUST guarantee safe non-concurrent access, e.g. softirq */
	if (likely(pool->alloc.array->count)) {
		/* Fast-path */
		netmem = pool->alloc.array->array[--pool->alloc.array->count];
		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);

		return netmem;
	}

	if (likely(pool->alloc.array)) {
		cacheflow_page_pool_put_empty_mini_array(pool, pool->alloc.array);
		pool->alloc.array = NULL;
	}

	if (likely(pool->alloc.full_mini_array_count)) {
		cacheflow_page_pool_pop_mini_array(pool);
		goto recheck;
	} else {
		netmem = cacheflow_page_pool_refill_alloc_cache(pool);
	}

	return netmem;
}

static void __cacheflow_page_pool_dma_sync_for_device(const struct cacheflow_page_pool *pool, netmem_ref netmem, u32 dma_sync_size)
{
#if defined(CONFIG_HAS_DMA) && defined(CONFIG_DMA_NEED_SYNC)
	dma_addr_t dma_addr = cacheflow_page_pool_get_dma_addr_netmem(netmem);

	dma_sync_size = min(dma_sync_size, pool->p.max_len);
	__dma_sync_single_for_device(pool->p.dev, dma_addr + pool->p.offset,
				     dma_sync_size, pool->p.dma_dir);
#endif
}

static __always_inline void
cacheflow_page_pool_dma_sync_for_device(const struct cacheflow_page_pool *pool,
			      netmem_ref netmem,
			      u32 dma_sync_size)
{
	if (dma_dev_need_sync(pool->p.dev))
		__cacheflow_page_pool_dma_sync_for_device(pool, netmem, dma_sync_size);
}

static bool cacheflow_page_pool_dma_map(struct cacheflow_page_pool *pool, netmem_ref netmem)
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

	if (cacheflow_page_pool_set_dma_addr_netmem(netmem, dma))
		goto unmap_failed;

	cacheflow_page_pool_dma_sync_for_device(pool, netmem, pool->p.max_len);

	return true;

unmap_failed:
	WARN_ONCE(1, "unexpected DMA address, please report to netdev@");
	dma_unmap_page_attrs(pool->p.dev, dma,
			     PAGE_SIZE << pool->p.order, pool->p.dma_dir,
			     DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING);
	return false;
}

static struct page *__cacheflow_page_pool_alloc_page_order(struct cacheflow_page_pool *pool, gfp_t gfp)
{
	struct page *page;

	gfp |= __GFP_COMP;
	page = alloc_pages_node(pool->p.nid, gfp, pool->p.order);
	if (unlikely(!page))
		return NULL;

	if (unlikely(!cacheflow_page_pool_dma_map(pool, page_to_netmem(page)))) {
		put_page(page);
		return NULL;
	}

	cacheflow_page_pool_set_pp_info(pool, page_to_netmem(page));

	/* Track how many pages are held 'in-flight' */
	pool->pages_state_hold_cnt++;
	trace_cacheflow_page_pool_state_hold(pool, page_to_netmem(page),
				   pool->pages_state_hold_cnt);
	return page;
}

/* slow path */
static noinline netmem_ref __cacheflow_page_pool_alloc_pages_slow(struct cacheflow_page_pool *pool, gfp_t gfp)
{
	const int bulk = PP_ALLOC_CACHE_REFILL;
	unsigned int pp_order = pool->p.order;
	netmem_ref netmem;
	int i, nr_pages;

	/* Don't support bulk alloc for high-order pages */
	if (unlikely(pp_order)) {
		netmem = page_to_netmem(__cacheflow_page_pool_alloc_page_order(pool, gfp));
		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_UNALLOC, PAGE_POOL_ALLOC);
		return netmem;
	}

	/* Unnecessary as alloc cache is empty, but guarantees zero count */
	if (unlikely(pool->alloc.array->count > 0)) {
		netmem = pool->alloc.array->array[--pool->alloc.array->count];

		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);
		return netmem;
	}

	int j;
	for (i = 0; i < bulk / PP_ALLOC_CACHE_BULK; i++) {
		pool->alloc.array = cacheflow_page_pool_get_mini_array(pool);
		if (unlikely(!pool->alloc.array))
			goto out;

		pool->alloc.array->count = 0;

		memset(pool->alloc.array, 0, sizeof(void *) * PP_ALLOC_CACHE_BULK);

		nr_pages = alloc_pages_bulk_array_node(gfp,
						pool->p.nid, PP_ALLOC_CACHE_BULK,
						(struct page **)pool->alloc.array);

		if (unlikely(!nr_pages)) {
			cacheflow_page_pool_put_empty_mini_array(pool, pool->alloc.array);
			pool->alloc.array = 0;
			goto out;
		}

		/* Pages have been filled into alloc.cache array, but count is zero and
		* page element have not been (possibly) DMA mapped.
		*/
		for (j = 0; j < nr_pages; j++) {
			netmem = pool->alloc.array->array[j];
			if (unlikely(!cacheflow_page_pool_dma_map(pool, netmem))) {
				put_page(netmem_to_page(netmem));
				continue;
			}

			cacheflow_page_pool_set_pp_info(pool, netmem);
			pool->alloc.array->array[pool->alloc.array->count++] = netmem;

			/* Track how many pages are held 'in-flight' */
			pool->pages_state_hold_cnt++;
			trace_cacheflow_page_pool_state_hold(pool, netmem,
						pool->pages_state_hold_cnt);
			cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_UNALLOC, PAGE_POOL_ARRAY);
		}

		if (likely(pool->alloc.array->count == PP_ALLOC_CACHE_BULK)) {
			pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count++] = pool->alloc.array;
			pool->alloc.array = 0;
			pool->alloc.array->count = 0;
		} else if (unlikely(pool->alloc.array->count == 0)) {
			cacheflow_page_pool_put_empty_mini_array(pool, pool->alloc.array);
			pool->alloc.array = 0;
			break;
		} else {
			break;
		}
	}
out:
	if (pool->alloc.full_mini_array_count == 0) {
		cacheflow_page_pool_pop_mini_array(pool);
	}

	/* Return last page */
	if (likely(pool->alloc.array->count > 0)) {
		netmem = pool->alloc.array->array[--pool->alloc.array->count];

		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);
	} else {
		netmem = 0;
	}

	/* When page just alloc'ed is should/must have refcnt 1. */
	return netmem;
}

/* For using page_pool replace: alloc_pages() API calls, but provide
 * synchronization guarantee for allocation side.
 */
netmem_ref cacheflow_page_pool_alloc_netmem(struct cacheflow_page_pool *pool, gfp_t gfp)
{
	netmem_ref netmem = 0;

	/* Fast-path: Get a page from cache */
	netmem = __cacheflow_page_pool_get_cached(pool);
	if (netmem)
		return netmem;

	netmem = __cacheflow_page_pool_alloc_pages_slow(pool, gfp);
	return netmem;
}
EXPORT_SYMBOL(cacheflow_page_pool_alloc_netmem);

struct page *cacheflow_page_pool_alloc_pages(struct cacheflow_page_pool *pool, gfp_t gfp)
{
	return netmem_to_page(cacheflow_page_pool_alloc_netmem(pool, gfp));
}
EXPORT_SYMBOL(cacheflow_page_pool_alloc_pages);
ALLOW_ERROR_INJECTION(cacheflow_page_pool_alloc_pages, NULL);

/* Calculate distance between two u32 values, valid if distance is below 2^(31)
 *  https://en.wikipedia.org/wiki/Serial_number_arithmetic#General_Solution
 */
#define _distance(a, b)	(s32)((a) - (b))

static s32 cacheflow_page_pool_inflight(const struct cacheflow_page_pool *pool, bool strict)
{
	u32 release_cnt = atomic_read(&pool->pages_state_release_cnt);
	u32 hold_cnt = READ_ONCE(pool->pages_state_hold_cnt);
	s32 inflight;

	inflight = _distance(hold_cnt, release_cnt);

	if (strict) {
		// trace_page_pool_release(pool, inflight, hold_cnt, release_cnt);
		WARN(inflight < 0, "Negative(%d) inflight packet-pages",
		     inflight);
	} else {
		inflight = max(0, inflight);
	}

	return inflight;
}

void cacheflow_page_pool_set_pp_info(struct cacheflow_page_pool *pool, netmem_ref netmem)
{
	netmem_set_pp(netmem, pool);
	netmem_or_pp_magic(netmem, PP_SIGNATURE);
}

static void cacheflow_page_pool_clear_pp_info(netmem_ref netmem)
{
	netmem_clear_pp_magic(netmem);
	netmem_set_pp(netmem, NULL);
}

static __always_inline void __cacheflow_page_pool_release_page_dma(struct cacheflow_page_pool *pool,
							 netmem_ref netmem)
{
	dma_addr_t dma;

	dma = cacheflow_page_pool_get_dma_addr_netmem(netmem);

	/* When page is unmapped, it cannot be returned to our pool */
	dma_unmap_page_attrs(pool->p.dev, dma,
			     PAGE_SIZE << pool->p.order, pool->p.dma_dir,
			     DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING);
	cacheflow_page_pool_set_dma_addr_netmem(netmem, 0);
}

/* Disconnects a page (from a page_pool).  API users can have a need
 * to disconnect a page (from a page_pool), to allow it to be used as
 * a regular page (that will eventually be returned to the normal
 * page-allocator via put_page).
 */
static void cacheflow_page_pool_return_page(struct cacheflow_page_pool *pool, netmem_ref netmem)
{
	int count;

	__cacheflow_page_pool_release_page_dma(pool, netmem);

	/* This may be the last page returned, releasing the pool, so
	 * it is not safe to reference pool afterwards.
	 */
	count = atomic_inc_return_relaxed(&pool->pages_state_release_cnt);
	// trace_page_pool_state_release(pool, netmem, count);

	cacheflow_page_pool_clear_pp_info(netmem);
	put_page(netmem_to_page(netmem));
	/* An optimization would be to call __free_pages(page, pool->p.order)
	 * knowing page is not part of page-cache (thus avoiding a
	 * __page_cache_release() call).
	 */
}

/* Only allow direct recycling in special circumstances, into the
 * alloc side cache.  E.g. during RX-NAPI processing for XDP_DROP use-case.
 *
 * Caller must provide appropriate safe context.
 */
static bool cacheflow_page_pool_recycle_in_cache(netmem_ref netmem,
				       		struct cacheflow_page_pool *pool)
{
	if (pool->alloc.array->count == PP_ALLOC_CACHE_BULK) {
		cacheflow_page_pool_push_mini_array(pool);
	}

	/* Caller MUST have verified/know (page_ref_count(page) == 1) */
	pool->alloc.array->array[pool->alloc.array->count++] = netmem;

	cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_ARRAY);
	return true;
}

static bool __cacheflow_page_pool_page_can_be_recycled(netmem_ref netmem)
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
__cacheflow_page_pool_put_page(struct cacheflow_page_pool *pool, netmem_ref netmem,
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
	if (likely(__cacheflow_page_pool_page_can_be_recycled(netmem))) {
		/* Read barrier done in page_ref_count / READ_ONCE */

		cacheflow_page_pool_dma_sync_for_device(pool, netmem, dma_sync_size);

		if (allow_direct && cacheflow_page_pool_recycle_in_cache(netmem, pool)) {
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
	cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
	cacheflow_page_pool_return_page(pool, netmem);

	return 0;
}

static bool cacheflow_page_pool_napi_local(const struct cacheflow_page_pool *pool)
{
	const struct napi_struct *napi;
	u32 cpuid;

	if (unlikely(!in_softirq())) {
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
		return true;
	}

	napi = READ_ONCE(pool->p.napi);

	if (napi && READ_ONCE(napi->list_owner) == cpuid) {
		return true;
	}

	return false;
}

void cacheflow_page_pool_put_netmem(struct cacheflow_page_pool *pool, netmem_ref netmem,
				  unsigned int dma_sync_size, bool allow_direct)
{
	if (!allow_direct)
		allow_direct = cacheflow_page_pool_napi_local(pool);
	
	netmem = __cacheflow_page_pool_put_page(pool, netmem, dma_sync_size, allow_direct);

	if (netmem) {
		if (!ptr_ring_produce_bh(&pool->recycle_ring, (__force void *)netmem))
			return;
		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
		cacheflow_page_pool_return_page(pool, netmem);
	}
}
EXPORT_SYMBOL(cacheflow_page_pool_put_netmem);

void cacheflow_page_pool_put_page(struct cacheflow_page_pool *pool, struct page *page,
				unsigned int dma_sync_size, bool allow_direct)
{
	cacheflow_page_pool_put_netmem(pool, page_to_netmem(page), dma_sync_size,
				     allow_direct);
}
EXPORT_SYMBOL(cacheflow_page_pool_put_page);

static void cacheflow_page_pool_empty_ring(struct cacheflow_page_pool *pool)
{
	struct netmem_mini_array* mini_array;
	int i;

	while ((mini_array = ptr_stack_pop_bh(&pool->stack))) 
	{
		cacheflow_page_pool_account_usages(pool, mini_array->array, mini_array->count, PAGE_POOL_RING, PAGE_POOL_UNALLOC);
		for (i = 0; i < mini_array->count; i++) {
			if (!(netmem_ref_count(mini_array->array[i]) == 1))
				pr_crit("%s() page_pool refcnt %d violation\n",
					__func__, netmem_ref_count(mini_array->array[i]));
			cacheflow_page_pool_return_page(pool, (__force netmem_ref)mini_array->array[i]);
		}
		cacheflow_page_pool_put_empty_mini_array(pool, mini_array);
	}
}

static void __cacheflow_page_pool_destroy(struct cacheflow_page_pool *pool)
{
#ifdef CONFIG_NET_CACHEFLOW_DEBUG
	if (pool->allocated_pages || pool->array_pages || pool->ring_pages) {
		pr_err("page_pool: accounting error, allocated_pages=%u, array_pages=%u, ring_pages=%u\n",
			pool->allocated_pages, pool->array_pages, pool->ring_pages);
		BUG();
	}
#endif

	if (pool->disconnect)
		pool->disconnect(pool);

	cacheflow_page_pool_uninit(pool);

	kfree(pool);
}

static void cacheflow_page_pool_empty_mini_array(struct cacheflow_page_pool *pool, struct netmem_mini_array* mini_array)
{
	int i;
	if (unlikely(!mini_array))
		return;

	cacheflow_page_pool_account_usages(pool, mini_array->array, mini_array->count, PAGE_POOL_ARRAY, PAGE_POOL_UNALLOC);
	for (i = 0; i < mini_array->count; i++) {
		cacheflow_page_pool_return_page(pool, (__force netmem_ref)mini_array->array[i]);
	}
	mini_array->count = 0;
	cacheflow_page_pool_put_empty_mini_array(pool, mini_array);
}

static void cacheflow_page_pool_empty_alloc_cache_once(struct cacheflow_page_pool *pool)
{
	if (pool->destroy_cnt)
		return;

	/* Empty alloc cache, assume caller made sure this is
	 * no-longer in use, and page_pool_alloc_pages() cannot be
	 * call concurrently.
	 */
	do {
		cacheflow_page_pool_empty_mini_array(pool, pool->alloc.array);
		pool->alloc.array = NULL;
	} while (cacheflow_page_pool_pop_mini_array(pool));

}

static void cacheflow_page_pool_scrub_recycle_ring(struct cacheflow_page_pool *pool) {
	netmem_ref netmem;

	while((netmem = (netmem_ref)__ptr_ring_consume(&pool->recycle_ring))) {
		cacheflow_page_pool_return_page(pool, netmem);
		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
	}
}

static void cacheflow_page_pool_scrub(struct cacheflow_page_pool *pool)
{
	cacheflow_page_pool_empty_alloc_cache_once(pool);
	pool->destroy_cnt++;

	cacheflow_page_pool_scrub_recycle_ring(pool);

	/* No more consumers should exist, but producers could still
	 * be in-flight.
	 */
	cacheflow_page_pool_empty_ring(pool);
}

static int cacheflow_page_pool_release(struct cacheflow_page_pool *pool)
{
	int inflight;

	cacheflow_page_pool_scrub(pool);
	inflight = cacheflow_page_pool_inflight(pool, true);
	if (!inflight)
		__cacheflow_page_pool_destroy(pool);

	return inflight;
}

static void cacheflow_page_pool_release_retry(struct work_struct *wq)
{
	struct delayed_work *dwq = to_delayed_work(wq);
	struct cacheflow_page_pool *pool = container_of(dwq, typeof(*pool), release_dw);
	void *netdev;
	int inflight;

	inflight = cacheflow_page_pool_release(pool);
	if (!inflight)
		return;

	/* Periodic warning for page pools the user can't see */
	netdev = READ_ONCE(pool->slow.netdev);
	if (time_after_eq(jiffies, pool->defer_warn) &&
	    (!netdev || netdev == NET_PTR_POISON)) {
		int sec = (s32)((u32)jiffies - (u32)pool->defer_start) / HZ;

		pr_warn("%s() stalled pool shutdown: id %px, %d inflight %d sec\n",
			__func__, pool, inflight, sec);
		pool->defer_warn = jiffies + DEFER_WARN_INTERVAL;
	}

	/* Still not ready to be disconnected, retry later */
	schedule_delayed_work(&pool->release_dw, DEFER_TIME);
}

void cacheflow_page_pool_destroy(struct cacheflow_page_pool *pool)
{
	if (!pool)
		return;

	if (!cacheflow_page_pool_put(pool))
		return;

	if (!cacheflow_page_pool_release(pool))
		return;

	pool->defer_start = jiffies;
	pool->defer_warn  = jiffies + DEFER_WARN_INTERVAL;

	INIT_DELAYED_WORK(&pool->release_dw, cacheflow_page_pool_release_retry);
	schedule_delayed_work(&pool->release_dw, DEFER_TIME);
}
EXPORT_SYMBOL(cacheflow_page_pool_destroy);


void cacheflow_page_pool_recycle_ring(struct cacheflow_page_pool *pool) {
	netmem_ref netmem;

	if (!cacheflow_page_pool_napi_local(pool))
		BUG();

	while((netmem = (netmem_ref)__ptr_ring_consume(&pool->recycle_ring))) {
		cacheflow_page_pool_put_netmem(pool, netmem, -1, true);
	}
}

static int __init netmem_bulk_cache_init(void)
{
	netmem_mini_array_cache = kmem_cache_create("netmem_bulk_cache",
		sizeof(struct netmem_mini_array), 0, 
		SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);
	return 0;
}

core_initcall(netmem_bulk_cache_init);
