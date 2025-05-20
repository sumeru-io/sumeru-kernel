/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CACHEFLOW_PAGE_POOL_PRIV_H
#define __CACHEFLOW_PAGE_POOL_PRIV_H

#include <net/netmem.h>
#include <net/cacheflow/page_pool.h>

static inline unsigned long netmem_get_pp_magic(netmem_ref netmem)
{
	return __netmem_clear_lsb(netmem)->pp_magic;
}

static inline void netmem_or_pp_magic(netmem_ref netmem, unsigned long pp_magic)
{
	__netmem_clear_lsb(netmem)->pp_magic |= pp_magic;
}

static inline void netmem_clear_pp_magic(netmem_ref netmem)
{
	__netmem_clear_lsb(netmem)->pp_magic = 0;
}

static inline void netmem_set_pp(netmem_ref netmem, struct cacheflow_page_pool *pool)
{
	__netmem_clear_lsb(netmem)->cacheflow_pp = pool;
}

static inline void netmem_set_dma_addr(netmem_ref netmem,
				       unsigned long dma_addr)
{
	__netmem_clear_lsb(netmem)->dma_addr = dma_addr;
}

static inline bool
cacheflow_page_pool_set_dma_addr_netmem(netmem_ref netmem, dma_addr_t addr)
{
	if (PAGE_POOL_32BIT_ARCH_WITH_64BIT_DMA) {
		netmem_set_dma_addr(netmem, addr >> PAGE_SHIFT);

		/* We assume page alignment to shave off bottom bits,
		 * if this "compression" doesn't work we need to drop.
		 */
		return addr != (dma_addr_t)netmem_get_dma_addr(netmem)
				       << PAGE_SHIFT;
	}

	netmem_set_dma_addr(netmem, addr);
	return false;
}

static inline bool cacheflow_page_pool_set_dma_addr(struct page *page, dma_addr_t addr)
{
	return cacheflow_page_pool_set_dma_addr_netmem(page_to_netmem(page), addr);
}
#endif /* __CACHEFLOW_PAGE_POOL_PRIV_H */
