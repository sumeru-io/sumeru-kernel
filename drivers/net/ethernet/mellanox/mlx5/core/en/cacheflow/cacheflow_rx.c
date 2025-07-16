// SPDX-License-Identifier: GPL-2.0
#include <net/cacheflow/cacheflow.h>
#include <net/cacheflow/page_pool.h>
#include <net/rps.h>
#include <trace/events/cacheflow.h>
#include <trace/events/skb.h>

#include "en/cacheflow/cacheflow.h"
#include "en/cacheflow/rq_tracker.h"
#include "en/txrx.h"

#include "trace/events/skb.h"
#include "diag/cacheflow_tracepoint.h"

static int mlx5e_cacheflow_get_cpu(u32 hash)
{
	const struct rps_sock_flow_table *sock_flow_table;
	u32 ident;

	sock_flow_table = rcu_dereference(net_hotdata.rps_sock_flow_table);
	if (sock_flow_table) {
		ident = READ_ONCE(
			sock_flow_table->ents[hash & sock_flow_table->mask]);
		if ((ident ^ hash) & ~net_hotdata.rps_cpu_mask)
			return 0;
  
		return rps_core(ident & net_hotdata.rps_cpu_mask);
	}

	return 0;
}

static void mlx5e_cacheflow_handle_rx_cqe(struct mlx5e_cacheflow_rq *rq,
					  struct mlx5_cqe64 *cqe,
					  struct mlx5e_cacheflow_cqe *cacheflow_cqe)
{
	struct mlx5e_cacheflow *cacheflow =
		container_of(rq, struct mlx5e_cacheflow, rq);
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	struct mlx5e_rq_stats *stats = rq->stats;
	struct page **wi;
	u32 cqe_bcnt;
	u16 ci;
	u64 cacheflow_id = 0;

	WARN_ON(rq->wqe.info.log_num_frags != 0);
	WARN_ON(rq->wqe.info.num_frags != 1);

	ci = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
	wi = &rq->wqe.frags[ci];
	cqe_bcnt = be32_to_cpu(cqe->byte_cnt);

	if (cacheflow->rq_tracker) {		
		cacheflow_id = mlx5e_cacheflow_rq_tracker_update(cacheflow->rq_tracker,						  
						mlx5e_cqe_ts_to_ns(rq->ptp_cyc2time, rq->clock,
					       get_cqe_ts(cqe)),
						ktime_get_real_ns());
	}

	memcpy(&cacheflow_cqe->cqe, cqe, sizeof(struct mlx5_cqe64));
	cacheflow_cqe->cqe.cacheflow.page_addr_high = (u64)(*wi) >> 32;
	cacheflow_cqe->cqe.cacheflow.page_addr_low = (u64)(*wi) & 0xffffffff;

	trace_skb_cacheflow_memory_location(page_to_netmem(*wi), NETMEM_LOCATION_NAPI);
	// trace_mlx5e_cacheflow_bh_cqe(rq->ix, cqe_bcnt, *wi,
				//      tcpu);
	*wi = NULL;

	stats->packets++;
	stats->bytes += cqe_bcnt;
}

static noinline int mlx5e_cacheflow_bh_poll(struct mlx5e_cacheflow *c,
					    int budget)
{
	struct mlx5e_cacheflow_rq *rq = &c->rq;
	struct mlx5e_cq *cq = &c->rq.cq;
	struct mlx5_cqwq *cqwq = &cq->wq;
	struct mlx5_cqe64 *cqe;
	struct mlx5e_cacheflow_cqe *cacheflow_cqes;

	int cpu, work_done = 0, tcpu, n, i, j;
	u64 current_time;

	if (unlikely(!test_bit(MLX5E_RQ_STATE_ENABLED, &rq->state)))
		return 0;

	while (work_done < budget && (cqe = mlx5_cqwq_get_cqe(cqwq))) {
		mlx5_cqwq_pop(cqwq);
		work_done++;

		prefetch(cqe + 1);
		prefetch(cqe + 2);
		prefetch(cqe + 3);

		if (unlikely(MLX5E_RX_ERR_CQE(cqe))) {
			rq->stats->wqe_err++;
			pr_info("cacheflow: wqe error, op_code=%d\n",
				get_cqe_opcode(cqe));
			continue;
		}

		tcpu = mlx5e_cacheflow_get_cpu(be32_to_cpu(cqe->rss_hash_result));

		__cpumask_set_cpu(tcpu, &c->notify_cpu_set);
		__cpumask_set_cpu(tcpu, &c->cqes_cpu_set);
		c->cqes[tcpu][c->cqes_nums[tcpu]++] = cqe;
	}

	for_each_cpu(cpu, &c->cqes_cpu_set) {
		n = item_ring_reserve_n(c->th_array[cpu].cqe_ring, c->cqes_nums[cpu], (void **)&cacheflow_cqes);
		for (i = 0; i < n; i++) {
			mlx5e_cacheflow_handle_rx_cqe(rq, c->cqes[cpu][i], cacheflow_cqes + i);
			c->th_array[cpu].inserted++;
		}
		item_ring_submit_n(c->th_array[cpu].cqe_ring, n);

		if (n < c->cqes_nums[cpu]) {
			n += item_ring_reserve_n(c->th_array[cpu].cqe_ring, c->cqes_nums[cpu] - n, (void **)&cacheflow_cqes);
			for (j = 0; i < n; i++, j++) {
				mlx5e_cacheflow_handle_rx_cqe(rq, c->cqes[cpu][i], cacheflow_cqes + j);
			}
			item_ring_submit_n(c->th_array[cpu].cqe_ring, j);
		}

		for (i = n; i < c->cqes_nums[cpu]; i++) {
			pr_info("cacheflow: missed cqe, cpu=%d\n", cpu);
			c->th_array[cpu].missed++;
		}
		c->cqes_nums[cpu] = 0;
		__cpumask_clear_cpu(cpu, &c->cqes_cpu_set);
	}

	if (work_done == 0)
		goto skip_update;

	mlx5_wq_cyc_pop_n(&rq->wqe.wq, work_done);

	mlx5_cqwq_update_db_record(cqwq);

	/* ensure cq space is freed before enabling more cqes */
	wmb();

skip_update:
	current_time = ktime_to_us(ktime_get());

	for_each_cpu(cpu, &c->notify_cpu_set) {
		if (current_time - c->th_array[cpu].last_scheduled_time >
		    get_cacheflow_ipi_usec_thresh()) {
			if (!cmpxchg(&c->th_array[cpu].ipi_scheduled, 0, 1)) {
				smp_call_function_single_async(
					cpu, &c->th_array[cpu].csd);
				trace_mlx5e_cacheflow_th_ipi_scheduled(
					cpu, current_time,
					c->th_array[cpu].last_scheduled_time,
					item_ring_items_available(
						c->th_array[cpu].cqe_ring));
			}
			c->th_array[cpu].last_scheduled_time =
			current_time;
			__cpumask_clear_cpu(cpu, &c->notify_cpu_set);
		}
	} 

	// we request to be polled again if we have pending signal to send
	return work_done ? : !cpumask_empty(&c->notify_cpu_set);
}

int mlx5e_cacheflow_bh_napi_poll(struct napi_struct *napi, int budget)
{
	struct mlx5e_cacheflow *c =
		container_of(napi, struct mlx5e_cacheflow, napi);
	struct mlx5e_ch_stats *ch_stats = c->stats;
	struct mlx5e_cacheflow_rq *rq = &c->rq;
	bool busy = false;
	int work_done = 0;

	rcu_read_lock();

	ch_stats->poll++;

	if (unlikely(!budget))
		goto out;

	cacheflow_page_pool_recycle_ring(c->rq.page_pool);

	work_done = mlx5e_cacheflow_bh_poll(c, budget);

	busy |= work_done == budget;
	busy |= mlx5e_cacheflow_post_rx_wqes(rq);

	if (busy) {
		work_done = budget;
		goto out;
	}

	if (unlikely(!napi_complete_done(napi, work_done)))
		goto out;

	ch_stats->arm++;

	mlx5e_cq_arm(&rq->cq);
out:
	rcu_read_unlock();
	if (work_done)
		trace_cacheflow_napi_poll(napi->dev, smp_processor_id(),
					  work_done);
	return work_done;
}
