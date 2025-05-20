#include <net/cacheflow/cacheflow.h>
#include <net/cacheflow/page_pool.h>
#include <net/rps.h>
#include <trace/events/cacheflow.h>

#include "en/cacheflow/cacheflow.h"
#include "en/txrx.h"

#include "trace/events/skb.h"
#include "diag/cacheflow_tracepoint.h"

static int mlx5e_cacheflow_get_cpu(u32 hash)
{
	const struct rps_sock_flow_table *sock_flow_table;
	u32 ident;

	sock_flow_table = rcu_dereference(net_hotdata.rps_sock_flow_table);
	if (sock_flow_table) {
		ident = READ_ONCE(sock_flow_table->ents[hash & sock_flow_table->mask]);
		if ((ident ^ hash) & ~net_hotdata.rps_cpu_mask)
			return 0;

		return rps_core(ident & net_hotdata.rps_cpu_mask);
	}

	return 0;
}

static void mlx5e_cacheflow_handle_rx_cqe(struct mlx5e_cacheflow_rq *rq, struct mlx5_cqe64 *cqe)
{
	struct mlx5e_cacheflow* cacheflow = container_of(rq, struct mlx5e_cacheflow, rq);
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	struct page **wi;
	u32 cqe_bcnt;
	u16 ci;
	int tcpu;
	struct mlx5e_cacheflow_th *th;
	struct mlx5e_cacheflow_cqe *cacheflow_cqe;
	ktime_t timestamp;
	int i;

	ci       = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
	wi       = &rq->wqe.frags[ci << rq->wqe.info.log_num_frags];
	cqe_bcnt = be32_to_cpu(cqe->byte_cnt);

	if (cacheflow->rq_tracker) {
		timestamp = mlx5e_cqe_ts_to_ns(rq->ptp_cyc2time, rq->clock, get_cqe_ts(cqe));
		mlx5e_cacheflow_rq_tracker_update(cacheflow->rq_tracker, ktime_get(), timestamp);
	}

	if (unlikely(MLX5E_RX_ERR_CQE(cqe))) {
		rq->stats->wqe_err++;
		pr_info("cacheflow: wqe error, op_code=%d, \n", get_cqe_opcode(cqe));
		goto wq_cyc_pop;
	}
	tcpu = mlx5e_cacheflow_get_cpu(be32_to_cpu(cqe->rss_hash_result));
	th = &cacheflow->th_array[tcpu];

	cacheflow_cqe = item_ring_reserve(th->cqe_ring);

	if (unlikely(!cacheflow_cqe)) {
		pr_err("cacheflow: kfifo to core %d is full\n", tcpu);
		goto wq_cyc_pop;
	}

	memcpy(&cacheflow_cqe->cqe, cqe, sizeof(struct mlx5_cqe64));
	for (i = 0; i < rq->wqe.info.num_frags; i++) {
		cacheflow_cqe->page[i] = *wi;
		*wi = NULL;
		wi++;
	}

	trace_mlx5e_cacheflow_bh_cqe(rq->ix, cqe_bcnt, cacheflow_cqe->page, tcpu);

	item_ring_submit(th->cqe_ring);

	__cpumask_set_cpu(tcpu, &cacheflow->notify_cpu_set);

wq_cyc_pop:
	mlx5_wq_cyc_pop(wq);
}

static noinline int mlx5e_cacheflow_bh_poll(struct mlx5e_cacheflow *c, int budget)
{
	struct mlx5e_cacheflow_rq *rq = &c->rq;
	struct mlx5e_cq *cq = &c->rq.cq;
	struct mlx5_cqwq *cqwq = &cq->wq;
	struct mlx5_cqe64 *cqe;
	int cpu, work_done = 0;
	u64 current_time;

	if (unlikely(!test_bit(MLX5E_RQ_STATE_ENABLED, &rq->state)))
		return 0;

	while (work_done < budget && (cqe = mlx5_cqwq_get_cqe(cqwq))) {
		// it's almostly correct since cqes are packed on pages.
		prefetch(cqe + 1);
		mlx5_cqwq_pop(cqwq);
		mlx5e_cacheflow_handle_rx_cqe(rq, cqe);
		work_done++;
	}

	if (work_done == 0)
		return 0;

	mlx5_cqwq_update_db_record(cqwq);

	/* ensure cq space is freed before enabling more cqes */
	wmb();

	current_time = ktime_to_us(ktime_get());

	for_each_cpu(cpu, &c->notify_cpu_set) {
		if ((current_time - c->th_array[cpu].last_scheduled_time > get_cacheflow_ipi_usec_thresh()) || (item_ring_items_available(c->th_array[cpu].cqe_ring) > get_cacheflow_ipi_packet_thresh())) {
			if (!cmpxchg(&c->th_array[cpu].ipi_scheduled, 0, 1)) {
				smp_call_function_single_async(cpu, &c->th_array[cpu].csd);
				trace_mlx5e_cacheflow_th_ipi_scheduled(cpu, current_time, c->th_array[cpu].last_scheduled_time, item_ring_items_available(c->th_array[cpu].cqe_ring));
				c->th_array[cpu].last_scheduled_time = current_time;
				__cpumask_clear_cpu(cpu, &c->notify_cpu_set);
			}
		}
	}

	return work_done;
}

int mlx5e_cacheflow_bh_napi_poll(struct napi_struct *napi, int budget)
{
	struct mlx5e_cacheflow *c = container_of(napi, struct mlx5e_cacheflow, napi);
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

	if (unlikely(!napi_complete_done(napi, work_done)))
		goto out;

	ch_stats->arm++;

	mlx5e_cq_arm(&rq->cq);
out:
	rcu_read_unlock();
	if (work_done)
		trace_cacheflow_napi_poll(napi->dev, smp_processor_id(), work_done);
	return work_done;
}