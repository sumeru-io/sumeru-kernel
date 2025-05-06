#include <net/cacheflow.h>
#include <trace/events/cacheflow.h>

#include "en/cacheflow.h"
#include "en/params.h"
#include "en/txrx.h"
struct mlx5e_cacheflow_params {
	struct mlx5e_params params;
	struct mlx5e_rq_param rq_param;
};

static void mlx5e_cacheflow_build_rq_param(struct mlx5_core_dev *mdev,
					  struct net_device *netdev,
					  struct mlx5e_cacheflow_params *cparams)
{
	struct mlx5e_params *params = &cparams->params;
	struct mlx5e_rq_param *rq_param = &cparams->rq_param;

	params->rq_wq_type = MLX5_WQ_TYPE_CYCLIC;
	params->log_rq_mtu_frames = cacheflow_channel_descriptor;

	mlx5e_build_rq_param(mdev, params, NULL, rq_param);
	rq_param->frags_info.wqe_bulk = max_t(u16, rq_param->frags_info.wqe_index_mask + 1, 8);
	rq_param->frags_info.refill_unit = rq_param->frags_info.wqe_bulk;

	rq_param->cacheflow_channel = 1;
}

static void mlx5e_cacheflow_build_params(struct mlx5e_cacheflow *c, 
				     	struct mlx5e_cacheflow_params *cparams,
					struct mlx5e_params *orig)
{
	struct mlx5e_params *params = &cparams->params;

	params->tx_min_inline_mode = orig->tx_min_inline_mode;
	params->num_channels = orig->num_channels;
	params->hard_mtu = orig->hard_mtu;
	params->sw_mtu = orig->sw_mtu;
	params->mqprio = orig->mqprio;

	return mlx5e_cacheflow_build_rq_param(c->mdev, c->netdev, cparams);
}

static int mlx5e_cacheflow_napi_poll(struct napi_struct *napi, int budget)
{
	struct mlx5e_cacheflow *c = container_of(napi, struct mlx5e_cacheflow, napi);
	struct mlx5e_ch_stats *ch_stats = c->stats;
	struct mlx5e_rq *rq = &c->rq;
	bool busy = false;
	int work_done = 0;

	rcu_read_lock();

	ch_stats->poll++;

	if (unlikely(!budget))
		goto out;

	if (likely(budget - work_done))
		work_done = mlx5e_poll_rx_cq(&rq->cq, budget);

	busy |= work_done == budget;
	busy |= INDIRECT_CALL_2(rq->post_wqes,
				mlx5e_post_rx_mpwqes,
				mlx5e_post_rx_wqes,
				rq);

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

static int mlx5e_cacheflow_open_rx_cq(struct mlx5e_cacheflow *c, struct mlx5e_cacheflow_params *cparams)
{
	int err;
	struct dim_cq_moder moder = {};
	struct mlx5e_create_cq_param ccp = {
		.netdev = c->netdev,
		.wq = c->priv->wq,
		.napi = &c->napi,
		.ch_stats = c->stats,
		.node = dev_to_node(mlx5_core_dma_dev(c->mdev)),
		.ix = 0,
	};

	err = mlx5e_open_cq(c->mdev, moder, &cparams->rq_param.cqp, &ccp, &c->rq.cq);

	return err;
}

static int mlx5e_cacheflow_init_rq(struct mlx5e_cacheflow *c, struct mlx5e_params *params, struct mlx5e_rq *rq) {
	struct mlx5_core_dev *mdev = c->mdev;
	struct mlx5e_priv *priv = c->priv;

	rq->wq_type      = params->rq_wq_type;
	rq->pdev         = c->pdev;
	rq->netdev       = priv->netdev;
	rq->priv         = priv;
	rq->clock        = &mdev->clock;
	rq->tstamp       = &priv->tstamp;
	rq->mdev         = mdev;
	rq->hw_mtu       = MLX5E_SW2HW_MTU(params, params->sw_mtu);
	rq->stats        = &c->priv->cacheflow_stats.rq;
	rq->ix           = 0;
	rq->ptp_cyc2time = mlx5_rq_ts_translator(mdev);

	return mlx5e_rq_set_handlers(rq, params, false);
}

static int mlx5e_cacheflow_open_rq(struct mlx5e_cacheflow *c, struct mlx5e_params *params,
			     struct mlx5e_rq_param *rq_param)
{
	int node = dev_to_node(mlx5_core_dma_dev(c->mdev));
	int err, sd_ix;
	u16 q_counter;

	sd_ix = mlx5_sd_ch_ix_get_dev_ix(c->mdev, 0);
	q_counter = c->priv->q_counter[sd_ix];

	err = mlx5e_cacheflow_init_rq(c, params, &c->rq);
	if (err)
		return err;

	sd_ix = mlx5_sd_ch_ix_get_dev_ix(c->mdev, 0);
	q_counter = c->priv->q_counter[sd_ix];
	pr_info("cacheflow: open rq on node %d, q_counter %d\n", node, q_counter);

	return mlx5e_open_rq(params, rq_param, NULL, node, q_counter, &c->rq);
}

static int mlx5e_cacheflow_open_queues(struct mlx5e_cacheflow *c, struct mlx5e_cacheflow_params *cparams)
{
	int err;

	err = mlx5e_cacheflow_open_rx_cq(c, cparams);
	if (err)
		return err;

	err = mlx5e_cacheflow_open_rq(c, &cparams->params, &cparams->rq_param);
	if (err)
		goto close_rx_cq;

	return 0;

close_rx_cq:
	mlx5e_close_cq(&c->rq.cq);

	return err;
}

static void mlx5e_cacheflow_close_queues(struct mlx5e_cacheflow *c)
{
	mlx5e_close_rq(&c->rq);
	mlx5e_close_cq(&c->rq.cq);
}

static void mlx5e_cacheflow_print_params(struct mlx5e_cacheflow_params *cparams)
{
	struct mlx5e_params *params = &cparams->params;
	struct mlx5e_rq_param *rq_param = &cparams->rq_param;

	pr_info("cacheflow channel params:\n");
	pr_info("  mlx5e_params: log_sq_size=%u, rq_wq_type=%u, log_rq_mtu_frames=%u, num_channels=%u\n",
		params->log_sq_size, params->rq_wq_type, params->log_rq_mtu_frames, params->num_channels);
	pr_info("  mqprio: mode=%u, num_tc=%u\n",
		params->mqprio.mode, params->mqprio.num_tc);
	pr_info("  cqe_compress_def=%d, vlan_strip_disable=%d, scatter_fcs=%d\n",
		params->rx_cqe_compress_def, params->vlan_strip_disable, params->scatter_fcs_en);
	pr_info("  dim: rx_en=%d, tx_en=%d, rx_use_cqe=%d, tx_use_cqe=%d\n",
		params->rx_dim_enabled, params->tx_dim_enabled, params->rx_moder_use_cqe_mode, params->tx_moder_use_cqe_mode);
	pr_info("  packet_merge: type=%d, timeout=%u, shampo(match_type=%u, align_gran=%u)\n",
		params->packet_merge.type, params->packet_merge.timeout,
		params->packet_merge.shampo.match_criteria_type, params->packet_merge.shampo.alignment_granularity);
	pr_info("  tx_min_inline=%u, pflags=0x%x, sw_mtu=%u, hard_mtu=%d, ptp_rx=%d, lkey=0x%x\n",
		params->tx_min_inline_mode, params->pflags, params->sw_mtu, params->hard_mtu,
		params->ptp_rx, be32_to_cpu(params->terminate_lkey_be));

	pr_info("  mlx5e_rq_param:\n");
	pr_info("    cqp: eq_ix=%u, cq_period_mode=%u, cqc_size=%zu\n",
		rq_param->cqp.eq_ix, rq_param->cqp.cq_period_mode, sizeof(rq_param->cqp.cqc));
	pr_info("      cqp.wq: buf_numa_node=%d, db_numa_node=%d\n",
		rq_param->cqp.wq.buf_numa_node, rq_param->cqp.wq.db_numa_node);
	pr_info("    rqc_size: %zu\n", sizeof(rq_param->rqc));
	pr_info("    wq: buf_numa_node=%d, db_numa_node=%d\n",
		rq_param->wq.buf_numa_node, rq_param->wq.db_numa_node);
	pr_info("    frags_info: num_frags=%u, log_num_frags=%u, wqe_bulk=%u, refill_unit=%u, wqe_index_mask=0x%x\n",
		rq_param->frags_info.num_frags, rq_param->frags_info.log_num_frags,
		rq_param->frags_info.wqe_bulk, rq_param->frags_info.refill_unit,
		rq_param->frags_info.wqe_index_mask);
	if (rq_param->frags_info.num_frags > 0) {
		pr_info("      frags_info.arr[0]: frag_size=%d, frag_stride=%d\n",
			rq_param->frags_info.arr[0].frag_size, rq_param->frags_info.arr[0].frag_stride);
	}
	for (int i = 0; i < rq_param->frags_info.num_frags; i++) {
		pr_info("      frags_info.arr[%d]: frag_size=%d, frag_stride=%d\n",
			i, rq_param->frags_info.arr[i].frag_size, rq_param->frags_info.arr[i].frag_stride);
	}
	pr_info("    xdp_frag_size=%u, cacheflow_channel=%u\n",
		rq_param->xdp_frag_size, rq_param->cacheflow_channel);
}

int mlx5e_cacheflow_open(struct mlx5e_priv *priv, struct mlx5e_params *params,
			 u8 lag_port, struct mlx5e_cacheflow **cc) {

	struct net_device *netdev = priv->netdev;
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_cacheflow_params *cparams;
	struct mlx5e_cacheflow *c;
	int err;

	c = kvzalloc_node(sizeof(*c), GFP_KERNEL, dev_to_node(mlx5_core_dma_dev(mdev)));
	cparams = kvzalloc(sizeof(*cparams), GFP_KERNEL);
	if (!c || !cparams) {
		err = -ENOMEM;
		goto err_free;
	}

	c->priv = priv;
	c->mdev = mdev;
	c->netdev = netdev;
	c->pdev = mlx5_core_dma_dev(mdev);
	c->netdev = priv->netdev;
	c->mkey_be = cpu_to_be32(priv->mdev->mlx5e_res.hw_objs.mkey);
	c->num_tc = mlx5e_get_dcb_num_tc(params);
	c->stats = &priv->cacheflow_stats.ch;
	c->lag_port = lag_port;

	mlx5e_cacheflow_build_params(c, cparams, params);
	mlx5e_cacheflow_print_params(cparams);

	int core = get_cacheflow_steer_core();
	netif_cacheflow_napi_add_weight(netdev, &c->napi, mlx5e_cacheflow_napi_poll, 16, core);
	pr_info("cacheflow: add NAPI (kthread) on core %d, res: %s\n", 
		core, test_bit(NAPI_STATE_CACHEFLOW, &c->napi.state) ? "succeed" : "fail");

	err = mlx5e_cacheflow_open_queues(c, cparams);
	if (unlikely(err))
		goto err_napi_del;

	priv->cacheflow_opened = true;

	*cc = c;

	kvfree(cparams);
	return 0;

err_napi_del:
	netif_napi_del(&c->napi);
err_free:
	kvfree(cparams);
	kvfree(c);
	return err;
}

void mlx5e_cacheflow_close(struct mlx5e_cacheflow *c)
{
	mlx5e_cacheflow_close_queues(c);
	netif_napi_del(&c->napi);

	kvfree(c);
}

void mlx5e_cacheflow_activate_channel(struct mlx5e_cacheflow *c)
{
	napi_enable(&c->napi);

	mlx5e_activate_rq(&c->rq);

	mlx5e_trigger_napi_sched(&c->napi);
}

void mlx5e_cacheflow_deactivate_channel(struct mlx5e_cacheflow *c)
{
	mlx5e_deactivate_rq(&c->rq);
	napi_disable(&c->napi);
}
