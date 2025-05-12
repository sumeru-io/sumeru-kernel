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

/* Size array to fit within two cachelines minus the count field */
#define CF_PP_MINI_ARRAY_METADATA_SIZE				8
#define CF_PP_MINI_ARRAY_SIZE 					(((2 * L1_CACHE_BYTES) - CF_PP_MINI_ARRAY_METADATA_SIZE) / sizeof(netmem_ref))
#define CF_PP_FULL_MINI_ARRAY_CACHE_SIZE			16
#define CF_PP_MINI_ARRAY_REFILL_BATCH_SIZE			(CF_PP_FULL_MINI_ARRAY_CACHE_SIZE / 2)
#define CF_PP_EMPTY_MINI_ARRAY_FREE_CACHE_SIZE			(CF_PP_FULL_MINI_ARRAY_CACHE_SIZE * 2)

struct netmem_mini_array {
	netmem_ref array[CF_PP_MINI_ARRAY_SIZE];
	int count;
	int flags;
} ____cacheline_aligned_in_smp;

struct cacheflow_pp_alloc_cache {
	struct netmem_mini_array* mini_array;

	struct netmem_mini_array* full_mini_array_cache[CF_PP_FULL_MINI_ARRAY_CACHE_SIZE];
	u32 full_mini_array_count;

	struct netmem_mini_array* empty_mini_array_cache[CF_PP_EMPTY_MINI_ARRAY_FREE_CACHE_SIZE];
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

struct cacheflow_page_pool {
	struct cacheflow_page_pool_params_fast p;

	int cpuid;
	u32 pages_state_hold_cnt;

	struct ptr_ring recycle_ring;

	struct delayed_work release_dw;
	void (*disconnect)(void *pool);
	unsigned long defer_start;
	unsigned long defer_warn;

	u32 array_pages;
	u32 ring_pages;
	u32 allocated_pages;
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

	u64 destroy_cnt;

	/* Slow/Control-path information follows */
	struct cacheflow_page_pool_params_slow slow;
};

struct cacheflow_page_pool *cacheflow_page_pool_create(const struct cacheflow_page_pool_params *params);
struct cacheflow_page_pool *cacheflow_page_pool_create_percpu(const struct cacheflow_page_pool_params *params, int cpuid);
void cacheflow_page_pool_destroy(struct cacheflow_page_pool *pool);

struct page *cacheflow_page_pool_alloc_pages(struct cacheflow_page_pool *pool, gfp_t gfp);
netmem_ref cacheflow_page_pool_alloc_netmem(struct cacheflow_page_pool *pool, gfp_t gfp);
void cacheflow_page_pool_alloc_pages_bulk(struct cacheflow_page_pool *pool, void **data, int count);

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

#endif /* __CACHEFLOW_PAGE_POOL_H */