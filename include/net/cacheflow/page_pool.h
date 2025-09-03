/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_PAGE_POOL_H
#define __CACHEFLOW_PAGE_POOL_H

#include <linux/dma-mapping.h>
#include <linux/dma-direction.h>
#include <linux/ptr_ring.h>
#include <linux/ptr_stack.h>
#include <linux/types.h>
#include <linux/cache.h> /* For L1_CACHE_BYTES */

#include <net/net_debug.h>
#include <net/netmem.h>
#include <net/cacheflow/netmem_array.h>

/*
 * Fast allocation side cache array/stack
 *
 * The cache size and refill watermark is related to the network
 * use-case.  The NAPI budget is 64 packets.  After a NAPI poll the RX
 * ring is usually refilled and the max consumed elements will be 64,
 * thus a natural max size of objects needed in the cache.
 *
 * Keeping room for more objects, is due to XDP_DROP use-case.  As
 * XDP_DROP allows the opportunity to recycle objects directly into
 * this array, as it shares the same softirq/NAPI protection.  If
 * cache is already full (or partly full) then the XDP_DROP recycles
 * would have to take a slower code path.
 */

extern struct kmem_cache *netmem_mini_array_cache;


#define DEFAULT_CF_PP_FULL_MINI_ARRAY_CACHE_SIZE		256
#define DEFAULT_CF_PP_EMPTY_MINI_ARRAY_FREE_CACHE_SIZE		(DEFAULT_CF_PP_FULL_MINI_ARRAY_CACHE_SIZE * 2)
#define CF_PP_MINI_ARRAY_REFILL_BATCH_SIZE			8

struct cacheflow_pp_alloc_cache {
	struct netmem_mini_array* partial_array;

#ifndef CONFIG_NET_CACHEFLOW_BUFFER_ANNEAL
	/* Simple LIFO cache when anneal queue is disabled */
	struct netmem_mini_array** full_mini_array_cache;
	u32 full_mini_array_cache_size;
	u32 full_mini_array_count;
#else
	/* Advanced per-core buffer annealing system */
	struct anneal_queue anneal_queue ____cacheline_aligned_in_smp;
#endif

	struct netmem_mini_array** empty_mini_array_cache;
	u32 empty_mini_array_cache_size;
	u32 empty_mini_array_count;
};

/**
 * struct page_pool_params - page pool parameters
 * @fast:	params accessed frequently on hotpath
 * @order:	2^order pages on allocation
 * @pool_size:	size of the ptr_ring
 * @nid:	NUMA node id to allocate from pages from
 * @dev:	device, for DMA pre-mapping purposes
 * @napi:	NAPI which is the sole consumer of pages, otherwise NULL
 * @dma_dir:	DMA mapping direction
 * @max_len:	max DMA sync memory size for PP_FLAG_DMA_SYNC_DEV
 * @offset:	DMA sync address offset for PP_FLAG_DMA_SYNC_DEV
 * @slow:	params with slowpath access only (initialization and Netlink)
 * @netdev:	netdev this pool will serve (leave as NULL if none or multiple)
 * @queue_idx:	queue idx this page_pool is being created for.
 */
struct cacheflow_page_pool_params {
	struct_group_tagged(cacheflow_page_pool_params_fast, fast,
		unsigned int	order;
		unsigned int	pool_size;
		unsigned int	anneal_size;
		int		nid;
		struct device	*dev;
		struct napi_struct *napi;
		enum dma_data_direction dma_dir;
		unsigned int	max_len;
		unsigned int	offset;
	);
	struct_group_tagged(cacheflow_page_pool_params_slow, slow,
		struct net_device *netdev;
		unsigned int queue_idx;
	);
};

#define PAGE_POOL_NAME_MAX_LEN (64)
struct cacheflow_page_pool_proc {
	char page_pool_name[PAGE_POOL_NAME_MAX_LEN];
	struct page_pool *pool;
    	struct proc_dir_entry *proc_dir;
    	struct proc_dir_entry *stats_file;
	struct proc_dir_entry *watermark_file;
};

#define CACHEFLOW_TH_EMPTY_MINI_ARRAY_CACHE_SIZE 16

struct cacheflow_page_pool_recycle_stub {
	struct cacheflow_page_pool *pool;
	struct netmem_mini_array *mini_array;

	struct netmem_mini_array *mini_array_cache[CACHEFLOW_TH_EMPTY_MINI_ARRAY_CACHE_SIZE];
	int mini_array_cache_count;
};

struct cacheflow_page_pool {
	struct cacheflow_page_pool_params_fast p;

	int cpuid;
	u32 pages_state_hold_cnt;

	struct ptr_ring recycle_ring;
	atomic_t oob_recycle_cnt;

	struct cacheflow_page_pool_recycle_stub __percpu *recycle_stub;

	struct delayed_work release_dw;
	void (*disconnect)(void *pool);
	unsigned long defer_start;
	unsigned long defer_warn;

	u32 array_pages;
	u32 ring_pages;
	u32 allocated_pages;

	struct delayed_work usage_track_work;

	struct page_pool_proc *proc;

	struct cacheflow_pp_alloc_cache alloc ____cacheline_aligned_in_smp;

	struct ptr_stack stack;

#ifdef CONFIG_PAGE_POOL_STATS
	/* recycle stats are per-cpu to avoid locking */
	struct page_pool_recycle_stats __percpu *recycle_stats;
#endif

	atomic_t pages_state_release_cnt;

	/* A page_pool is strictly tied to a single RX-queue being
	 * protected by NAPI, due to above pp_alloc_cache. This
	 * refcnt serves purpose is to simplify drivers error handling.
	 */
	refcount_t user_cnt;

	/* Slow/Control-path information follows */
	struct cacheflow_page_pool_params_slow slow;
};

struct cacheflow_page_pool *cacheflow_page_pool_create(const struct cacheflow_page_pool_params *params);
struct cacheflow_page_pool *cacheflow_page_pool_create_percpu(const struct cacheflow_page_pool_params *params, int cpuid);
void cacheflow_page_pool_destroy(struct cacheflow_page_pool *pool);

struct page *cacheflow_page_pool_alloc_pages(struct cacheflow_page_pool *pool, gfp_t gfp);
netmem_ref cacheflow_page_pool_alloc_netmem(struct cacheflow_page_pool *pool, gfp_t gfp);
struct netmem_mini_array *cacheflow_page_pool_get_full_mini_array(struct cacheflow_page_pool *pool, gfp_t gfp);
void cacheflow_page_pool_put_empty_mini_array(struct cacheflow_page_pool *pool, struct netmem_mini_array *mini_array);

void cacheflow_page_pool_put_netmem(struct cacheflow_page_pool *pool,
					netmem_ref netmem,
					unsigned int dma_sync_size,
					bool allow_direct);
void cacheflow_page_pool_put_page(struct cacheflow_page_pool *pool, struct page *page, unsigned int dma_sync_size, bool allow_direct);
void cacheflow_page_pool_put_page_bulk(struct cacheflow_page_pool *pool, void **data, int count);

void cacheflow_page_pool_recycle_ring(struct cacheflow_page_pool *pool);

/**
 * page_pool_dev_alloc_pages() - allocate a page.
 * @pool:	pool from which to allocate
 *
 * Get a page from the page allocator or page_pool caches.
 */
static inline struct page *cacheflow_page_pool_dev_alloc_pages(struct cacheflow_page_pool *pool)
{
	gfp_t gfp = (GFP_ATOMIC | __GFP_NOWARN);

	return cacheflow_page_pool_alloc_pages(pool, gfp);
}

/**
 * page_pool_get_dma_dir() - Retrieve the stored DMA direction.
 * @pool:	pool from which page was allocated
 *
 * Get the stored dma direction. A driver might decide to store this locally
 * and avoid the extra cache line from page_pool to determine the direction.
 */
static inline enum dma_data_direction
cacheflow_page_pool_get_dma_dir(const struct cacheflow_page_pool *pool)
{
	return pool->p.dma_dir;
}

#define PAGE_POOL_32BIT_ARCH_WITH_64BIT_DMA	\
		(sizeof(dma_addr_t) > sizeof(unsigned long))

static inline dma_addr_t cacheflow_page_pool_get_dma_addr_netmem(netmem_ref netmem)
{
	dma_addr_t ret = netmem_get_dma_addr(netmem);

	if (PAGE_POOL_32BIT_ARCH_WITH_64BIT_DMA)
		ret <<= PAGE_SHIFT;

	return ret;
}

/**
 * page_pool_get_dma_addr() - Retrieve the stored DMA address.
 * @page:	page allocated from a page pool
 *
 * Fetch the DMA address of the page. The page pool to which the page belongs
 * must had been created with PP_FLAG_DMA_MAP.
 */
static inline dma_addr_t cacheflow_page_pool_get_dma_addr(const struct page *page)
{
	return cacheflow_page_pool_get_dma_addr_netmem(page_to_netmem((struct page *)page));
}

static inline bool cacheflow_page_pool_put(struct cacheflow_page_pool *pool)
{
	return refcount_dec_and_test(&pool->user_cnt);
}

void cacheflow_page_pool_set_pp_info(struct cacheflow_page_pool *pool, netmem_ref netmem);
int cacheflow_page_pool_alloc_empty_mini_array_bulk(struct netmem_mini_array **array, int count, gfp_t gfp);
void cacheflow_page_pool_free_empty_mini_array_bulk(struct netmem_mini_array **array, int count);

/* Backend-specific functions for mini array management */
void cacheflow_page_pool_push_full_mini_array(struct cacheflow_page_pool *pool, struct netmem_mini_array *mini_array);
struct netmem_mini_array *cacheflow_page_pool_pop_full_mini_array(struct cacheflow_page_pool *pool);
void cacheflow_page_pool_recycle_full_mini_array(struct cacheflow_page_pool *pool);

/* Anneal queue backend functions */
struct netmem_mini_array *cacheflow_anneal_queue_dequeue_full_mini_array(struct cacheflow_page_pool *pool);
void cacheflow_anneal_queue_enqueue_full_mini_array(struct cacheflow_page_pool *pool, struct netmem_mini_array *mini_array);

/* Unified interface - chooses backend based on CONFIG */
static inline struct netmem_mini_array *cacheflow_get_full_mini_array(struct cacheflow_page_pool *pool)
{
#ifdef CONFIG_NET_CACHEFLOW_BUFFER_ANNEAL
	return cacheflow_anneal_queue_dequeue_full_mini_array(pool);
#else
	return cacheflow_page_pool_pop_full_mini_array(pool);
#endif
}

static inline void cacheflow_put_full_mini_array(struct cacheflow_page_pool *pool, struct netmem_mini_array *mini_array)
{
#ifdef CONFIG_NET_CACHEFLOW_BUFFER_ANNEAL
	cacheflow_anneal_queue_enqueue_full_mini_array(pool, mini_array);
#else
	cacheflow_page_pool_push_full_mini_array(pool, mini_array);
#endif
}

static inline bool cacheflow_is_cache_full(struct cacheflow_page_pool *pool)
{
#ifdef CONFIG_NET_CACHEFLOW_BUFFER_ANNEAL
	/* Anneal queue has global overflow handling, never "full" */
	return false;
#else
	return pool->alloc.full_mini_array_count >= pool->alloc.full_mini_array_cache_size;
#endif
}

static inline bool cacheflow_is_cache_empty(struct cacheflow_page_pool *pool)
{
#ifdef CONFIG_NET_CACHEFLOW_BUFFER_ANNEAL
	return anneal_queue_is_empty(&pool->alloc.anneal_queue);
#else
	return pool->alloc.full_mini_array_count == 0;
#endif
}

static inline u32 cacheflow_get_cache_count(struct cacheflow_page_pool *pool)
{
#ifdef CONFIG_NET_CACHEFLOW_BUFFER_ANNEAL
	return anneal_queue_count(&pool->alloc.anneal_queue);
#else
	return pool->alloc.full_mini_array_count;
#endif
}

#endif /* __CACHEFLOW_PAGE_POOL_H */