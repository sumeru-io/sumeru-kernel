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
	struct mlx5e_cacheflow_wqe_frag_info *wi;
	u32 cqe_bcnt;
	u16 ci;
	int tcpu;
	struct mlx5e_cacheflow_th *th;
	struct mlx5e_cacheflow_cqe cacheflow_cqe;
	int i;
	int n;

	ci       = mlx5_wq_cyc_ctr2ix(wq, be16_to_cpu(cqe->wqe_counter));
	wi       = &rq->wqe.frags[ci << rq->wqe.info.log_num_frags];
	cqe_bcnt = be32_to_cpu(cqe->byte_cnt);

	if (unlikely(MLX5E_RX_ERR_CQE(cqe))) {
		rq->stats->wqe_err++;
		goto wq_cyc_pop;
	}

	memcpy(&cacheflow_cqe.cqe, cqe, sizeof(struct mlx5_cqe64));
	for (i = 0; i < rq->wqe.info.num_frags; i++) {
		cacheflow_cqe.page[i] = wi->page;
		wi->page = NULL;
		wi++;
	}

	tcpu = mlx5e_cacheflow_get_cpu(be32_to_cpu(cqe->rss_hash_result));
	th = &cacheflow->th_array[tcpu];

	trace_mlx5e_cacheflow_bh_cqe(rq->ix, cqe_bcnt, cacheflow_cqe.page, tcpu);

	cpumask_set_cpu(tcpu, &cacheflow->notify_cpu_set);

	n = kfifo_in(&th->cqe_fifo, &cacheflow_cqe, 1);

	if (n != 1) {
		pr_err("cacheflow: kfifo_in core %d returns %d, len=%d, size=%d\n", tcpu, n, kfifo_len(&th->cqe_fifo), kfifo_size(&th->cqe_fifo));
	}

wq_cyc_pop:
	mlx5_wq_cyc_pop(wq);
}

static int mlx5e_cacheflow_bh_poll_rx_cq(struct mlx5e_cacheflow *c, int budget)
{
	struct mlx5e_cacheflow_rq *rq = &c->rq;
	struct mlx5e_cq *cq = &c->rq.cq;
	struct mlx5_cqwq *cqwq = &cq->wq;
	struct mlx5_cqe64 *cqe;
	int cpu, work_done = 0;

	if (unlikely(!test_bit(MLX5E_RQ_STATE_ENABLED, &rq->state)))
		return 0;

	while (work_done < budget && (cqe = mlx5_cqwq_get_cqe(cqwq))) {
		mlx5_cqwq_pop(cqwq);
		mlx5e_cacheflow_handle_rx_cqe(rq, cqe);
		work_done++;
	}

	if (work_done == 0)
		return 0;

	mlx5_cqwq_update_db_record(cqwq);

	/* ensure cq space is freed before enabling more cqes */
	wmb();

	for_each_cpu(cpu, &c->notify_cpu_set) {
		if (!cmpxchg(&c->th_array[cpu].ipi_scheduled, 0, 1)) {
			smp_call_function_single_async(cpu, &c->th_array[cpu].csd);
			cpumask_clear_cpu(cpu, &c->notify_cpu_set);
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

	if (likely(budget - work_done))
		work_done = mlx5e_cacheflow_bh_poll_rx_cq(c, budget);

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