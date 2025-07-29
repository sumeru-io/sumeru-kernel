// SPDX-License-Identifier: GPL-2.0
/*
 *
 *	Author: Minhu Wang <minhuw@acm.org>
 *	Modified from standard Linux page_pool.c
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

#include <trace/events/cacheflow.h>

#include "page_pool.h"

#define DEFER_TIME (msecs_to_jiffies(1000))
#define DEFER_WARN_INTERVAL (60 * HZ)

#define BIAS_MAX (LONG_MAX >> 1)

struct kmem_cache *netmem_mini_array_cache;
static struct netmem_empty_mini_array_global_cache netmem_empty_mini_array_global_cache;

int cacheflow_page_pool_alloc_empty_mini_array_bulk(struct netmem_mini_array **array, int count, gfp_t gfp)
{
	spin_lock(&netmem_empty_mini_array_global_cache.lock);

	if (likely(netmem_empty_mini_array_global_cache.count >= count)) {
		netmem_empty_mini_array_global_cache.count -= count;
		memcpy(array, netmem_empty_mini_array_global_cache.array + netmem_empty_mini_array_global_cache.count, count * sizeof(struct netmem_mini_array*));
		spin_unlock(&netmem_empty_mini_array_global_cache.lock);
		return count;
	}

	spin_unlock(&netmem_empty_mini_array_global_cache.lock);
	return kmem_cache_alloc_bulk(netmem_mini_array_cache, gfp, count, (void **)array);
}

void cacheflow_page_pool_free_empty_mini_array_bulk(struct netmem_mini_array **array, int count)
{
	spin_lock(&netmem_empty_mini_array_global_cache.lock);
	if (likely(netmem_empty_mini_array_global_cache.count + count <= CF_PP_EMPTY_MINI_ARRAY_GLBOAL_CACHE_SIZE)) {
		memcpy(netmem_empty_mini_array_global_cache.array + netmem_empty_mini_array_global_cache.count, array, count * sizeof(struct netmem_mini_array*));
		netmem_empty_mini_array_global_cache.count += count;
		spin_unlock(&netmem_empty_mini_array_global_cache.lock);
		return;
	}

	spin_unlock(&netmem_empty_mini_array_global_cache.lock);
	kmem_cache_free_bulk(netmem_mini_array_cache, count, (void **)array);
}

enum {
	PAGE_POOL_ALLOC,
	PAGE_POOL_RING,
	PAGE_POOL_ARRAY,
	PAGE_POOL_UNALLOC,
};

static inline int
cacheflow_page_pool_account_usages(struct cacheflow_page_pool *pool,
				   netmem_ref *netmem, int n, int old_state,
				   int new_state)
{
	if (unlikely(n == 0))
		return 0;

	if (old_state == PAGE_POOL_ALLOC) {
#if IS_ENABLED(CONFIG_NET_CACHEFLOW_DEBUG)
		if (unlikely(pool->allocated_pages == 0)) {
			pr_err("page_pool: alloc pages = %u, array_pages = %u, ring_pages = %u\n",
			       pool->allocated_pages, pool->array_pages,
			       pool->ring_pages);
			BUG();
		}
#endif
		pool->allocated_pages -= n;
	} else if (old_state == PAGE_POOL_ARRAY) {
#if IS_ENABLED(CONFIG_NET_CACHEFLOW_DEBUG)
		if (unlikely(pool->array_pages == 0)) {
			pr_err("page_pool: alloc pages = %u, array_pages = %u, ring_pages = %u\n",
			       pool->allocated_pages, pool->array_pages,
			       pool->ring_pages);
			BUG();
		}
#endif
		pool->array_pages -= n;
	} else if (old_state == PAGE_POOL_RING) {
#if IS_ENABLED(CONFIG_NET_CACHEFLOW_DEBUG)
		if (unlikely(pool->ring_pages == 0)) {
			pr_err("page_pool: alloc pages = %u, array_pages = %u, ring_pages = %u\n",
			       pool->allocated_pages, pool->array_pages,
			       pool->ring_pages);
			BUG();
		}
#endif
		pool->ring_pages -= n;
	}

	if (new_state == PAGE_POOL_ALLOC)
		pool->allocated_pages += n;
	else if (new_state == PAGE_POOL_ARRAY)
		pool->array_pages += n;
	else if (new_state == PAGE_POOL_RING)
		pool->ring_pages += n;

	if (tracepoint_enabled(cacheflow_page_pool_page_move)) {
		int i;

		for (i = 0; i < n; i++) {
			trace_cacheflow_page_pool_page_move(
				pool, netmem[i], old_state, new_state,
				pool->allocated_pages, pool->array_pages,
				pool->ring_pages);
		}
	}

	return 0;
}

static inline int
cacheflow_page_pool_account_usage(struct cacheflow_page_pool *pool,
				  netmem_ref netmem, int old_state,
				  int new_state)
{
	return cacheflow_page_pool_account_usages(pool, &netmem, 1, old_state,
						  new_state);
}

static void cacheflow_page_pool_return_page(struct cacheflow_page_pool *pool,
					    netmem_ref netmem);

void
cacheflow_page_pool_put_empty_mini_array(struct cacheflow_page_pool *pool,
					 struct netmem_mini_array *mini_array)
{
	int i;

	if (!kasan_mempool_poison_object(mini_array)) {
		pr_warn_once(
			"cacheflow: put_empty_mini_array: failed to poison\n");
		kmem_cache_free(netmem_mini_array_cache, mini_array);
		return;
	}

	if (pool->alloc.empty_mini_array_count ==
	    CF_PP_EMPTY_MINI_ARRAY_FREE_CACHE_SIZE) {
		pool->alloc.empty_mini_array_count -=
			CF_PP_FULL_MINI_ARRAY_CACHE_SIZE;
		for (i = 0; i < CF_PP_FULL_MINI_ARRAY_CACHE_SIZE; i++)
			kasan_mempool_unpoison_object(
				pool->alloc.empty_mini_array_cache
					[pool->alloc.empty_mini_array_count + i],
				kmem_cache_size(netmem_mini_array_cache));

		cacheflow_page_pool_free_empty_mini_array_bulk(pool->alloc.empty_mini_array_cache + pool->alloc.empty_mini_array_count, CF_PP_FULL_MINI_ARRAY_CACHE_SIZE);
	}
	pool->alloc
		.empty_mini_array_cache[pool->alloc.empty_mini_array_count++] =
		mini_array;
}
EXPORT_SYMBOL(cacheflow_page_pool_put_empty_mini_array);

static inline struct netmem_mini_array *
cacheflow_page_pool_get_empty_mini_array(struct cacheflow_page_pool *pool)
{
	int i, n;
	struct netmem_mini_array *mini_array;

	if (pool->alloc.empty_mini_array_count == 0) {
		n = cacheflow_page_pool_alloc_empty_mini_array_bulk(pool->alloc.empty_mini_array_cache + pool->alloc.empty_mini_array_count, CF_PP_FULL_MINI_ARRAY_CACHE_SIZE, GFP_ATOMIC | GFP_NOWAIT);

		for (i = 0; i < n; i++) {
			mini_array =
				pool->alloc.empty_mini_array_cache
					[pool->alloc.empty_mini_array_count + i];
			memset(mini_array, 0, sizeof(struct netmem_mini_array));
			kasan_mempool_poison_object(mini_array);
		}
		pool->alloc.empty_mini_array_count += n;
	}
	if (likely(pool->alloc.empty_mini_array_count > 0)) {
		mini_array = pool->alloc.empty_mini_array_cache
				     [pool->alloc.empty_mini_array_count - 1];
		pool->alloc.empty_mini_array_cache
			[pool->alloc.empty_mini_array_count] = NULL;
		pool->alloc.empty_mini_array_count--;
		kasan_mempool_unpoison_object(
			mini_array, kmem_cache_size(netmem_mini_array_cache));

		return mini_array;
	}
	return NULL;
}

static inline struct netmem_mini_array *
cacheflow_page_pool_pop_full_mini_array(struct cacheflow_page_pool *pool)
{
	struct netmem_mini_array *mini_array = NULL;
	if (likely(pool->alloc.full_mini_array_count)) {
#ifdef CONFIG_CACHEFLOW_WARM_BUFFER
		mini_array = pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_head];
		pool->alloc.full_mini_array_head = (pool->alloc.full_mini_array_head + 1) % CF_PP_FULL_MINI_ARRAY_CACHE_SIZE;
#else
		mini_array = pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count - 1];
		pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count - 1] = NULL;
#endif
		pool->alloc.full_mini_array_count--;
	}
	trace_cacheflow_queue_depth(pool->alloc.full_mini_array_count);
	return mini_array;
}

static inline struct netmem_mini_array *
cacheflow_page_pool_refill_full_mini_array(struct cacheflow_page_pool *pool)
{
	struct ptr_stack *r = &pool->stack;
	struct netmem_mini_array *mini_array;

	/* Quicker fallback, avoid locks when ring is empty */
	if (ptr_stack_empty(r))
		return 0;

	do {
		mini_array = (struct netmem_mini_array *)ptr_stack_pop(r);

		if (unlikely(!mini_array))
			break;

		cacheflow_page_pool_account_usages(pool, mini_array->array,
						   CF_PP_MINI_ARRAY_SIZE,
						   PAGE_POOL_RING,
						   PAGE_POOL_ARRAY);

#ifdef CONFIG_CACHEFLOW_WARM_BUFFER
		pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_tail] = mini_array;
		pool->alloc.full_mini_array_tail = (pool->alloc.full_mini_array_tail + 1) % CF_PP_FULL_MINI_ARRAY_CACHE_SIZE;
#else
		pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count] = mini_array;
#endif
		pool->alloc.full_mini_array_count++;
	} while (pool->alloc.full_mini_array_count <
		 CF_PP_MINI_ARRAY_REFILL_BATCH_SIZE);

	return cacheflow_page_pool_pop_full_mini_array(pool);
}

static struct page *
__cacheflow_page_pool_alloc_page_order(struct cacheflow_page_pool *pool,
				       gfp_t gfp);
static bool cacheflow_page_pool_dma_map(struct cacheflow_page_pool *pool,
					netmem_ref netmem);

static inline struct netmem_mini_array *
cacheflow_page_pool_alloc_full_mini_array(struct cacheflow_page_pool *pool, gfp_t gfp)
{
	unsigned int pp_order = pool->p.order;
	struct netmem_mini_array *mini_array = NULL;
	netmem_ref netmem = 0;
	int i, nr_pages;

	mini_array = cacheflow_page_pool_get_empty_mini_array(pool);
	if (unlikely(!mini_array))
		return NULL;
	memset(mini_array->array, 0, sizeof(void *) * CF_PP_MINI_ARRAY_SIZE);

	/* Don't support bulk alloc for high-order pages */
	if (unlikely(pp_order)) {
		for (nr_pages = 0; nr_pages < CF_PP_MINI_ARRAY_SIZE; nr_pages++) {
			mini_array->array[nr_pages] = page_to_netmem(
				__cacheflow_page_pool_alloc_page_order(pool, gfp));
			if (unlikely(!mini_array->array[nr_pages]))
				goto release;
		}

		cacheflow_page_pool_account_usages(pool, mini_array->array,
						   CF_PP_MINI_ARRAY_SIZE,
						   PAGE_POOL_UNALLOC,
						   PAGE_POOL_ARRAY);

		return mini_array;
	} else {
		nr_pages = alloc_pages_bulk_array_node(
			gfp, pool->p.nid, CF_PP_MINI_ARRAY_SIZE,
			(struct page **)mini_array->array);

		if (unlikely(nr_pages != CF_PP_MINI_ARRAY_SIZE)) {
			goto release;
		} else {
			/* Pages have been filled into alloc.cache array, but count is zero and
			* page element have not been (possibly) DMA mapped.
			*/
			for (i = 0; i < nr_pages; i++) {
				netmem = mini_array->array[i];

				if (unlikely(!cacheflow_page_pool_dma_map(pool,
									netmem))) {
					pr_warn("cacheflow: failed to dma map page %p\n",
						lowmem_page_address(
							netmem_to_page(netmem)));
					goto release;
				}

				cacheflow_page_pool_set_pp_info(pool, netmem);
			}

			/* Track how many pages are held 'in-flight' */
			pool->pages_state_hold_cnt += nr_pages;

			trace_cacheflow_page_pool_state_hold(
				pool, netmem, pool->pages_state_hold_cnt);

			for (i = 0; i < CF_PP_MINI_ARRAY_SIZE; i++) {
				trace_skb_cacheflow_memory_location(mini_array->array[i], NETMEM_LOCATION_POOL);
			}

			cacheflow_page_pool_account_usages(pool, mini_array->array,
							CF_PP_MINI_ARRAY_SIZE,
							PAGE_POOL_UNALLOC,
							PAGE_POOL_ARRAY);

			return mini_array;
		}
	}

release:
	for (i = 0; i < nr_pages; i++) {
		cacheflow_page_pool_return_page(pool, mini_array->array[i]);
		mini_array->array[i] = 0;
	}
	cacheflow_page_pool_put_empty_mini_array(pool, mini_array);
	return NULL;
}

static void
cacheflow_page_pool_recycle_full_mini_array(struct cacheflow_page_pool *pool)
{
	int i, j, ret;
	int free_n = pool->alloc.full_mini_array_count / 2;
	struct netmem_mini_array *mini_array;

#ifdef CONFIG_NET_CACHEFLOW_DEBUG
	if (unlikely(pool->alloc.full_mini_array_count !=
		     CF_PP_FULL_MINI_ARRAY_CACHE_SIZE))
		BUG();
#endif

	for (i = 0; i < free_n; i++) {
#ifdef CONFIG_CACHEFLOW_WARM_BUFFER
		mini_array = pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_head];
		pool->alloc.full_mini_array_head = (pool->alloc.full_mini_array_head + 1) % CF_PP_FULL_MINI_ARRAY_CACHE_SIZE;
#else
		mini_array = pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count - 1];
		pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count - 1] = NULL;
#endif
		pool->alloc.full_mini_array_count--;
		ret = __ptr_stack_push(&pool->stack,
				       (__force void *)mini_array);

		if (likely(!ret)) {
			cacheflow_page_pool_account_usages(
				pool, mini_array->array, CF_PP_MINI_ARRAY_SIZE,
				PAGE_POOL_ARRAY, PAGE_POOL_RING);
		} else {
			cacheflow_page_pool_account_usages(
				pool, mini_array->array, CF_PP_MINI_ARRAY_SIZE,
				PAGE_POOL_ARRAY, PAGE_POOL_UNALLOC);

			for (j = 0; j < CF_PP_MINI_ARRAY_SIZE; j++) {
				cacheflow_page_pool_return_page(
					pool, mini_array->array[j]);
				mini_array->array[j] = 0;
			}

			cacheflow_page_pool_put_empty_mini_array(pool,
								 mini_array);
		}
	}
}

static inline void
cacheflow_page_pool_push_full_mini_array(struct cacheflow_page_pool *pool, struct netmem_mini_array *mini_array)
{
	if (unlikely(pool->alloc.full_mini_array_count >=
		     CF_PP_FULL_MINI_ARRAY_CACHE_SIZE))
		cacheflow_page_pool_recycle_full_mini_array(pool);

#ifdef CONFIG_CACHEFLOW_WARM_BUFFER
	pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_tail] = mini_array;
	pool->alloc.full_mini_array_tail = (pool->alloc.full_mini_array_tail + 1) % CF_PP_FULL_MINI_ARRAY_CACHE_SIZE;
#else
	pool->alloc.full_mini_array_cache[pool->alloc.full_mini_array_count] = mini_array;
#endif
	pool->alloc.full_mini_array_count++;
}

static inline void cacheflow_recycle_page(struct cacheflow_page_pool *pool, netmem_ref netmem)
{
	if (is_cacheflow_steer_page_clear_enabled()) {
		void *addr = netmem_address(netmem);
		void *base = addr;
		BUG_ON(addr == NULL);

		// size_t page_size = PAGE_SIZE << pool->p.order;
		size_t page_size = 9000;
		while (addr < (void *)((unsigned long)base + page_size - 8 * L1_CACHE_BYTES)) {
			*(int *)addr = 0;
			*(int *)(addr + 2 * L1_CACHE_BYTES) = 0;
			*(int *)(addr + 4 * L1_CACHE_BYTES) = 0;
			*(int *)(addr + 6 * L1_CACHE_BYTES) = 0;
			addr += 8 * L1_CACHE_BYTES;
		}
	}
}

static inline void
cacheflow_page_pool_put_full_mini_array(struct cacheflow_page_pool *pool,
				   struct netmem_mini_array *mini_array)
{
	int i;
	for (i = 0; i < CF_PP_MINI_ARRAY_SIZE; i++) {
		cacheflow_recycle_page(pool, mini_array->array[i]);
		trace_skb_cacheflow_memory_location(mini_array->array[i], NETMEM_LOCATION_POOL);
	}

	cacheflow_page_pool_account_usages(pool, mini_array->array,
					   CF_PP_MINI_ARRAY_SIZE, PAGE_POOL_ALLOC,
					   PAGE_POOL_ARRAY);

	cacheflow_page_pool_push_full_mini_array(pool, mini_array);
}

static int
cacheflow_page_pool_init(struct cacheflow_page_pool *pool,
			 const struct cacheflow_page_pool_params *params,
			 int cpuid)
{
	unsigned int ring_qsize = 1024; /* Default */
	int i, cpu;

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

	pool->recycle_stub =
		alloc_percpu(struct cacheflow_page_pool_recycle_stub);
	if (!pool->recycle_stub) {
		pr_err("Failed to allocate per-cpu recycle_stub\n");
		return -ENOMEM;
	}

#ifdef CONFIG_PAGE_POOL_STATS
	pool->recycle_stats = alloc_percpu(struct page_pool_recycle_stats);
	if (!pool->recycle_stats)
		return -ENOMEM;
#endif

	if (ptr_stack_init(&pool->stack, ring_qsize, GFP_KERNEL) < 0) {
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

	pool->alloc.partial_array = (struct netmem_partial_mini_array *)cacheflow_page_pool_get_empty_mini_array(pool);
	pool->alloc.partial_array->count = 0;

	ptr_ring_init(&pool->recycle_ring, 1024, GFP_KERNEL);

	for_each_possible_cpu(cpu) {
		struct cacheflow_page_pool_recycle_stub *stub;

		stub = per_cpu_ptr(pool->recycle_stub, cpu);

		stub->pool = pool;
		stub->mini_array_cache_count = kmem_cache_alloc_bulk(
			netmem_mini_array_cache, GFP_KERNEL,
			CACHEFLOW_TH_EMPTY_MINI_ARRAY_CACHE_SIZE,
			(void **)stub->mini_array_cache);
		for (i = 0; i < stub->mini_array_cache_count; i++) {
			memset(stub->mini_array_cache[i], 0,
			       sizeof(struct netmem_mini_array));
			kasan_mempool_poison_object(stub->mini_array_cache[i]);
		}
		stub->mini_array = (struct netmem_partial_mini_array *)
			stub->mini_array_cache[--stub->mini_array_cache_count];
		kasan_mempool_unpoison_object(
			stub->mini_array,
			kmem_cache_size(netmem_mini_array_cache));
	}

	return 0;
}

static void cacheflow_page_pool_uninit(struct cacheflow_page_pool *pool)
{
	ptr_stack_cleanup(&pool->stack, NULL);

	put_device(pool->p.dev);

#ifdef CONFIG_PAGE_POOL_STATS
	free_percpu(pool->recycle_stats);
#endif
}

/**
 * page_pool_create_percpu() - create a page pool for a given cpu.
 * @params: parameters, see struct page_pool_params
 * @cpuid: cpu identifier
 */
struct cacheflow_page_pool *cacheflow_page_pool_create_percpu(
	const struct cacheflow_page_pool_params *params, int cpuid)
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
struct cacheflow_page_pool *
cacheflow_page_pool_create(const struct cacheflow_page_pool_params *params)
{
	return cacheflow_page_pool_create_percpu(params, -1);
}
EXPORT_SYMBOL(cacheflow_page_pool_create);

static void __cacheflow_page_pool_dma_sync_for_device(
	const struct cacheflow_page_pool *pool, netmem_ref netmem,
	u32 dma_sync_size)
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
					netmem_ref netmem, u32 dma_sync_size)
{
	if (dma_dev_need_sync(pool->p.dev))
		__cacheflow_page_pool_dma_sync_for_device(pool, netmem,
							  dma_sync_size);
}

static bool cacheflow_page_pool_dma_map(struct cacheflow_page_pool *pool,
					netmem_ref netmem)
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
	dma_unmap_page_attrs(pool->p.dev, dma, PAGE_SIZE << pool->p.order,
			     pool->p.dma_dir,
			     DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING);
	return false;
}

static struct page *
__cacheflow_page_pool_alloc_page_order(struct cacheflow_page_pool *pool,
				       gfp_t gfp)
{
	struct page *page;

	gfp |= __GFP_COMP;
	page = alloc_pages_node(pool->p.nid, gfp, pool->p.order);
	if (unlikely(!page))
		return NULL;

	if (unlikely(
		    !cacheflow_page_pool_dma_map(pool, page_to_netmem(page)))) {
		pr_warn("cacheflow: failed to dma map page %p\n",
			lowmem_page_address(page));
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

/* For using page_pool replace: alloc_pages() API calls, but provide
 * synchronization guarantee for allocation side.
 */
netmem_ref cacheflow_page_pool_alloc_netmem(struct cacheflow_page_pool *pool,
					    gfp_t gfp)
{
	netmem_ref netmem = 0;
	struct netmem_mini_array *mini_array;

	if (unlikely(pool->alloc.partial_array == NULL)) {
		return 0;
	}

	if (likely(pool->alloc.partial_array->count > 0)) {
		netmem = pool->alloc.partial_array->array[--pool->alloc.partial_array->count];
		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);
		return netmem;
	} 

	if (likely(mini_array = cacheflow_page_pool_get_full_mini_array(pool, gfp))) {
		cacheflow_page_pool_put_empty_mini_array(pool, (struct netmem_mini_array *)pool->alloc.partial_array);
		netmem = mini_array->array[CF_PP_MINI_ARRAY_SIZE - 1];
		cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ARRAY, PAGE_POOL_ALLOC);
		pool->alloc.partial_array = (struct netmem_partial_mini_array *)mini_array;
		pool->alloc.partial_array->count = CF_PP_MINI_ARRAY_SIZE - 1;
		return netmem;
	}

	return 0;
}
EXPORT_SYMBOL(cacheflow_page_pool_alloc_netmem);

struct page *cacheflow_page_pool_alloc_pages(struct cacheflow_page_pool *pool,
					     gfp_t gfp)
{
	return netmem_to_page(cacheflow_page_pool_alloc_netmem(pool, gfp));
}
EXPORT_SYMBOL(cacheflow_page_pool_alloc_pages);
ALLOW_ERROR_INJECTION(cacheflow_page_pool_alloc_pages, NULL);


struct netmem_mini_array *cacheflow_page_pool_get_full_mini_array(struct cacheflow_page_pool *pool, gfp_t gfp)
{
	struct netmem_mini_array *mini_array = NULL;

	mini_array = cacheflow_page_pool_pop_full_mini_array(pool);

	if (unlikely(!mini_array))
		mini_array = cacheflow_page_pool_refill_full_mini_array(pool);

	if (unlikely(!mini_array))
		mini_array = cacheflow_page_pool_alloc_full_mini_array(pool, gfp);

	if (likely(mini_array)) {
		cacheflow_page_pool_account_usages(pool, mini_array->array,
						   CF_PP_MINI_ARRAY_SIZE,
						   PAGE_POOL_ARRAY,
						   PAGE_POOL_ALLOC);
	}

	return mini_array;
}
EXPORT_SYMBOL(cacheflow_page_pool_get_full_mini_array);

/* Calculate distance between two u32 values, valid if distance is below 2^(31)
 *  https://en.wikipedia.org/wiki/Serial_number_arithmetic#General_Solution
 */
#define _distance(a, b) ((s32)((a) - (b)))

static s32 cacheflow_page_pool_inflight(const struct cacheflow_page_pool *pool,
					bool strict)
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

void cacheflow_page_pool_set_pp_info(struct cacheflow_page_pool *pool,
				     netmem_ref netmem)
{
	netmem_set_pp(netmem, pool);
	netmem_or_pp_magic(netmem, CACHEFLOW_PP_SIGNATURE);
}

static void cacheflow_page_pool_clear_pp_info(netmem_ref netmem)
{
	netmem_clear_pp_magic(netmem);
	netmem_set_pp(netmem, NULL);
}

static __always_inline void
__cacheflow_page_pool_release_page_dma(struct cacheflow_page_pool *pool,
				       netmem_ref netmem)
{
	dma_addr_t dma;

	dma = cacheflow_page_pool_get_dma_addr_netmem(netmem);

	/* When page is unmapped, it cannot be returned to our pool */
	dma_unmap_page_attrs(pool->p.dev, dma, PAGE_SIZE << pool->p.order,
			     pool->p.dma_dir,
			     DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING);
	cacheflow_page_pool_set_dma_addr_netmem(netmem, 0);
}

/* Disconnects a page (from a page_pool).  API users can have a need
 * to disconnect a page (from a page_pool), to allow it to be used as
 * a regular page (that will eventually be returned to the normal
 * page-allocator via put_page).
 */
static void cacheflow_page_pool_return_page(struct cacheflow_page_pool *pool,
					    netmem_ref netmem)
{
	int count;

	__cacheflow_page_pool_release_page_dma(pool, netmem);

	/* This may be the last page returned, releasing the pool, so
	 * it is not safe to reference pool afterwards.
	 */
	count = atomic_inc_return_relaxed(&pool->pages_state_release_cnt);
	trace_cacheflow_page_pool_state_release(pool, netmem, count);

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
static bool
cacheflow_page_pool_recycle_in_cache(netmem_ref netmem,
				     struct cacheflow_page_pool *pool)
{
	struct netmem_mini_array *mini_array;

	if (unlikely(pool->alloc.partial_array == NULL)) {
		return false;
	}

#ifdef CONFIG_NET_CACHEFLOW_DEBUG
	if (unlikely(pool->alloc.partial_array->count > (CF_PP_MINI_ARRAY_SIZE - 1) ||
		     pool->alloc.partial_array->count < 0))
		BUG();
#endif

	if (pool->alloc.partial_array->count == CF_PP_MINI_ARRAY_SIZE - 1) {
		mini_array = (struct netmem_mini_array *)pool->alloc.partial_array;
		mini_array->array[CF_PP_MINI_ARRAY_SIZE - 1] = netmem;
		cacheflow_page_pool_push_full_mini_array(pool, mini_array);
		pool->alloc.partial_array = (struct netmem_partial_mini_array *)cacheflow_page_pool_get_empty_mini_array(pool);
		pool->alloc.partial_array->count = 0;
	} else {
		pool->alloc.partial_array->array[pool->alloc.partial_array->count++] = netmem;
	}

	cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC,
					  PAGE_POOL_ARRAY);
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
static __always_inline netmem_ref __cacheflow_page_pool_put_page(
	struct cacheflow_page_pool *pool, netmem_ref netmem,
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

		cacheflow_page_pool_dma_sync_for_device(pool, netmem,
							dma_sync_size);

		if (allow_direct &&
		    cacheflow_page_pool_recycle_in_cache(netmem, pool))
			return 0;

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
	cacheflow_page_pool_account_usage(pool, netmem, PAGE_POOL_ALLOC,
					  PAGE_POOL_UNALLOC);
	cacheflow_page_pool_return_page(pool, netmem);

	return 0;
}

static bool
cacheflow_page_pool_napi_local(const struct cacheflow_page_pool *pool)
{
	const struct napi_struct *napi;
	u32 cpuid;

	if (unlikely(!in_softirq()))
		return false;

	/* Allow direct recycle if we have reasons to believe that we are
	 * in the same context as the consumer would run, so there's
	 * no possible race.
	 * __page_pool_put_page() makes sure we're not in hardirq context
	 * and interrupts are enabled prior to accessing the cache.
	 */
	cpuid = smp_processor_id();
	if (READ_ONCE(pool->cpuid) == cpuid)
		return true;

	napi = READ_ONCE(pool->p.napi);

	if (napi && READ_ONCE(napi->list_owner) == cpuid)
		return true;

	return false;
}

static int
cacheflow_page_pool_put_netmem_to_recycle_ring(struct cacheflow_page_pool *pool,
					       netmem_ref netmem)
{
	struct cacheflow_page_pool_recycle_stub *stub;
	struct netmem_mini_array *mini_array;
	int i, err = 0;
	int in_softirq = in_softirq();

	if (in_softirq) {
		stub = this_cpu_ptr(pool->recycle_stub);
	} else {
		local_bh_disable();
		stub = get_cpu_ptr(pool->recycle_stub);
	}

	if (unlikely(!stub->mini_array)) {
		err = -ENOMEM;
		goto out;
	}

	if (unlikely(stub->mini_array->count == CF_PP_MINI_ARRAY_SIZE - 1)) {
		mini_array = (struct netmem_mini_array *)stub->mini_array;
		mini_array->array[CF_PP_MINI_ARRAY_SIZE - 1] = netmem;

		if (unlikely(ptr_ring_produce_any(
			    &pool->recycle_ring,
			    (__force void *)mini_array))) {
			for (i = 0; i < CF_PP_MINI_ARRAY_SIZE; i++) {
				atomic_inc(&pool->oob_recycle_cnt);
				netmem = mini_array->array[i];
				trace_cacheflow_page_pool_page_move(
					pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC,
					pool->allocated_pages, pool->array_pages,
					pool->ring_pages);
				cacheflow_page_pool_return_page(pool, netmem);
			}
			kmem_cache_free(netmem_mini_array_cache, mini_array);
		}
		stub->mini_array = NULL;

		if (unlikely(!stub->mini_array_cache_count)) {
			stub->mini_array_cache_count = cacheflow_page_pool_alloc_empty_mini_array_bulk(stub->mini_array_cache, CACHEFLOW_TH_EMPTY_MINI_ARRAY_CACHE_SIZE, GFP_ATOMIC);

			for (i = 0; i < stub->mini_array_cache_count; i++) {
				memset(stub->mini_array_cache[i], 0,
				       sizeof(struct netmem_mini_array));
				kasan_mempool_poison_object(
					stub->mini_array_cache[i]);
			}

			if (unlikely(!stub->mini_array_cache_count)) {
				err = -ENOMEM;
				goto out;
			}
		}

		stub->mini_array = (struct netmem_partial_mini_array *)stub->mini_array_cache[--stub->mini_array_cache_count];
		kasan_mempool_unpoison_object(
			stub->mini_array,
			kmem_cache_size(netmem_mini_array_cache));
		stub->mini_array->count = 0;
	} else {
		stub->mini_array->array[stub->mini_array->count++] = netmem;
	}

out:
	if (!in_softirq) {
		put_cpu_ptr(stub);
		local_bh_enable();
	}

	return err;
}

void cacheflow_page_pool_put_netmem(struct cacheflow_page_pool *pool,
				    netmem_ref netmem,
				    unsigned int dma_sync_size,
				    bool allow_direct)
{
	if (!allow_direct)
		allow_direct = cacheflow_page_pool_napi_local(pool);

	netmem = __cacheflow_page_pool_put_page(pool, netmem, dma_sync_size,
						allow_direct);

	if (netmem) {
		if (likely(!cacheflow_page_pool_put_netmem_to_recycle_ring(pool, netmem)))
			return;

		atomic_inc(&pool->oob_recycle_cnt);
		trace_cacheflow_page_pool_page_move(
			pool, netmem, PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC,
			pool->allocated_pages, pool->array_pages,
			pool->ring_pages);
		cacheflow_page_pool_return_page(pool, netmem);
	}
}
EXPORT_SYMBOL(cacheflow_page_pool_put_netmem);

void cacheflow_page_pool_put_page(struct cacheflow_page_pool *pool,
				  struct page *page, unsigned int dma_sync_size,
				  bool allow_direct)
{
	cacheflow_page_pool_put_netmem(pool, page_to_netmem(page),
				       dma_sync_size, allow_direct);
}
EXPORT_SYMBOL(cacheflow_page_pool_put_page);

static void cacheflow_page_pool_empty_ring(struct cacheflow_page_pool *pool)
{
	struct netmem_mini_array *mini_array;
	int i;

	while ((mini_array = ptr_stack_pop_bh(&pool->stack))) {
		cacheflow_page_pool_account_usages(pool, mini_array->array,
						   CF_PP_MINI_ARRAY_SIZE,
						   PAGE_POOL_RING,
						   PAGE_POOL_UNALLOC);
		for (i = 0; i < CF_PP_MINI_ARRAY_SIZE; i++) {
			if (!(netmem_ref_count(mini_array->array[i]) == 1))
				pr_crit("%s() page_pool refcnt %d violation\n",
					__func__,
					netmem_ref_count(mini_array->array[i]));
			cacheflow_page_pool_return_page(
				pool, (__force netmem_ref)mini_array->array[i]);
			mini_array->array[i] = 0;
		}
		cacheflow_page_pool_put_empty_mini_array(pool, mini_array);
	}

	for (i = 0; i < pool->alloc.empty_mini_array_count; i++) {
		mini_array = pool->alloc.empty_mini_array_cache[i];
		kasan_mempool_unpoison_object(
			mini_array, kmem_cache_size(netmem_mini_array_cache));
	}
	kmem_cache_free_bulk(netmem_mini_array_cache,
			     pool->alloc.empty_mini_array_count,
			     (void **)pool->alloc.empty_mini_array_cache);
	pool->alloc.empty_mini_array_count = 0;
}

static void __cacheflow_page_pool_destroy(struct cacheflow_page_pool *pool)
{
#ifdef CONFIG_NET_CACHEFLOW_DEBUG
	if (pool->allocated_pages || pool->array_pages || pool->ring_pages ||
	    pool->alloc.full_mini_array_count ||
	    pool->alloc.empty_mini_array_count || pool->alloc.partial_array) {
		pr_err("page_pool: accounting error, allocated_pages=%u, array_pages=%u, ring_pages=%u\n",
		       pool->allocated_pages, pool->array_pages,
		       pool->ring_pages);
		pr_err("page_pool: full_mini_array_count=%u, empty_mini_array_count=%u, mini_array=%p\n",
			pool->alloc.full_mini_array_count, pool->alloc.empty_mini_array_count, pool->alloc.partial_array);
		pr_err("page_pool: hold_cnt=%u, inflight=%u\n",
			pool->pages_state_hold_cnt, atomic_read(&pool->pages_state_release_cnt));
		BUG();
	}
#endif
	free_percpu(pool->recycle_stub);

	if (pool->disconnect)
		pool->disconnect(pool);

	cacheflow_page_pool_uninit(pool);

	kfree(pool);
}

static void
cacheflow_page_pool_empty_mini_array(struct cacheflow_page_pool *pool,
				     struct netmem_mini_array *mini_array)
{
	int i;

	if (unlikely(!mini_array))
		return;

	cacheflow_page_pool_account_usages(pool, mini_array->array,
					   CF_PP_MINI_ARRAY_SIZE,
					   PAGE_POOL_ARRAY,
					   PAGE_POOL_UNALLOC);
	for (i = 0; i < CF_PP_MINI_ARRAY_SIZE; i++) {
		cacheflow_page_pool_return_page(
			pool, (__force netmem_ref)mini_array->array[i]);
		mini_array->array[i] = 0;
	}
	cacheflow_page_pool_put_empty_mini_array(pool, mini_array);
}

static void
cacheflow_page_pool_empty_alloc_cache(struct cacheflow_page_pool *pool)
{
	int i;
	struct netmem_mini_array *mini_array;

	if (pool->alloc.partial_array) {
		cacheflow_page_pool_account_usages(pool, pool->alloc.partial_array->array,
						pool->alloc.partial_array->count,
						PAGE_POOL_ARRAY,
						PAGE_POOL_UNALLOC);

		for (i = 0; i < pool->alloc.partial_array->count; i++) {
			cacheflow_page_pool_return_page(pool, pool->alloc.partial_array->array[i]);
			pool->alloc.partial_array->array[i] = 0;
		}
		pool->alloc.partial_array->count = 0;
		cacheflow_page_pool_put_empty_mini_array(pool, (struct netmem_mini_array *)pool->alloc.partial_array);
		pool->alloc.partial_array = NULL;
	}

	/* Empty alloc cache, assume caller made sure this is
	 * no-longer in use, and page_pool_alloc_pages() cannot be
	 * call concurrently.
	 */
	while ((mini_array = cacheflow_page_pool_pop_full_mini_array(pool))) {
		cacheflow_page_pool_empty_mini_array(pool, mini_array);
	}
}

static void
cacheflow_page_pool_scrub_recycle_ring(struct cacheflow_page_pool *pool)
{
	struct netmem_mini_array *mini_array;
	int i, cpu, freed_count;

	if ((freed_count = atomic_xchg(&pool->oob_recycle_cnt, 0)))
		pool->allocated_pages -= freed_count;

	while ((mini_array = (struct netmem_mini_array *)__ptr_ring_consume(
			&pool->recycle_ring))) {
		cacheflow_page_pool_account_usages(pool, mini_array->array,
						   CF_PP_MINI_ARRAY_SIZE,
						   PAGE_POOL_ALLOC,
						   PAGE_POOL_UNALLOC);
		for (i = 0; i < CF_PP_MINI_ARRAY_SIZE; i++) {
			cacheflow_page_pool_return_page(
				pool, (__force netmem_ref)mini_array->array[i]);
			mini_array->array[i] = 0;
		}
		kmem_cache_free(netmem_mini_array_cache, mini_array);
	}

	for_each_possible_cpu(cpu) {
		struct cacheflow_page_pool_recycle_stub *stub;

		stub = per_cpu_ptr(pool->recycle_stub, cpu);

		if (stub->mini_array) {
			for (i = 0; i < stub->mini_array->count; i++) {
				cacheflow_page_pool_return_page(
					stub->pool, stub->mini_array->array[i]);
				cacheflow_page_pool_account_usage(
					stub->pool, stub->mini_array->array[i],
					PAGE_POOL_ALLOC, PAGE_POOL_UNALLOC);
				stub->mini_array->array[i] = 0;
			}
			stub->mini_array->count = 0;
			kmem_cache_free(netmem_mini_array_cache,
					stub->mini_array);
			stub->mini_array = NULL;
		}

		for (i = 0; i < stub->mini_array_cache_count; i++)
			kasan_mempool_unpoison_object(
				stub->mini_array_cache[i],
				kmem_cache_size(netmem_mini_array_cache));

		kmem_cache_free_bulk(netmem_mini_array_cache,
				     stub->mini_array_cache_count,
				     (void **)stub->mini_array_cache);
		stub->mini_array_cache_count = 0;
	}
}

static void cacheflow_page_pool_scrub(struct cacheflow_page_pool *pool)
{
	cacheflow_page_pool_empty_alloc_cache(pool);

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

	pr_info("cacheflow: release the page pool %p, inflight %d, alloc %d, array %d, ring %d\n", pool, inflight, pool->allocated_pages, pool->array_pages, pool->ring_pages);
	if (!inflight)
		__cacheflow_page_pool_destroy(pool);

	return inflight;
}

static void cacheflow_page_pool_release_retry(struct work_struct *wq)
{
	struct delayed_work *dwq = to_delayed_work(wq);
	struct cacheflow_page_pool *pool =
		container_of(dwq, typeof(*pool), release_dw);
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

		pr_warn("%s() stalled pool shutdown: id %p, %d inflight %d sec\n",
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
	pool->defer_warn = jiffies + DEFER_WARN_INTERVAL;

	INIT_DELAYED_WORK(&pool->release_dw, cacheflow_page_pool_release_retry);
	schedule_delayed_work(&pool->release_dw, DEFER_TIME);
}
EXPORT_SYMBOL(cacheflow_page_pool_destroy);

void cacheflow_page_pool_recycle_ring(struct cacheflow_page_pool *pool)
{
	struct netmem_mini_array *mini_array;
	int freed_count;

	while ((mini_array = (struct netmem_mini_array *)__ptr_ring_consume(
			&pool->recycle_ring)))
		cacheflow_page_pool_put_full_mini_array(pool, mini_array);

	if ((freed_count = atomic_xchg(&pool->oob_recycle_cnt, 0)))
		pool->allocated_pages -= freed_count;
}
EXPORT_SYMBOL(cacheflow_page_pool_recycle_ring);

static int __init netmem_bulk_cache_init(void)
{
	netmem_mini_array_cache = kmem_cache_create(
		"netmem_bulk_cache", sizeof(struct netmem_mini_array), 0,
		SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);

	netmem_empty_mini_array_global_cache.count = kmem_cache_alloc_bulk(netmem_mini_array_cache, GFP_KERNEL, CF_PP_EMPTY_MINI_ARRAY_GLBOAL_CACHE_SIZE, (void **)netmem_empty_mini_array_global_cache.array);
	spin_lock_init(&netmem_empty_mini_array_global_cache.lock);
	return 0;
}

core_initcall(netmem_bulk_cache_init);
