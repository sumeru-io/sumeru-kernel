// SPDX-License-Identifier: GPL-2.0
#include <net/cacheflow/cacheflow.h>
#include <net/cacheflow/page_pool.h>
#include <net/rps.h>
#include <trace/events/cacheflow.h>
#include <linux/debugfs.h>

#include "en/cacheflow/cacheflow.h"
#include "en/cacheflow/rq_tracker.h"
#include "en/params.h"
#include "en/xdp.h"

#include "trace/events/skb.h"
#include "diag/cacheflow_tracepoint.h"
#define MLX5E_TC_FLOW_ID_MASK 0x0000ffff

struct mlx5e_cacheflow_params {
	struct mlx5e_params params;
	struct mlx5e_rq_param rq_param;
};

static inline struct page **get_frag(struct mlx5e_cacheflow_rq *rq, u16 ix)
{
	return &rq->wqe.frags[ix << rq->wqe.info.log_num_frags];
}

static void
mlx5e_cacheflow_build_rq_param(struct mlx5_core_dev *mdev,
			       struct net_device *netdev,
			       struct mlx5e_cacheflow_params *cparams)
{
	struct mlx5e_params *params = &cparams->params;
	struct mlx5e_rq_param *rq_param = &cparams->rq_param;

	params->rq_wq_type = MLX5_WQ_TYPE_CYCLIC;
	params->log_rq_mtu_frames = cacheflow_channel_descriptor;
	params->cacheflow = true;

	if (MLX5E_GET_PFLAG(params, MLX5E_PFLAG_DROPLESS_RQ))
		MLX5_SET(rqc, rq_param->rqc, delay_drop_en, 1);

	mlx5e_build_rq_param(mdev, params, NULL, rq_param);
	rq_param->frags_info.wqe_bulk =
		max_t(u16, rq_param->frags_info.wqe_index_mask + 1, 8);
	rq_param->frags_info.refill_unit = rq_param->frags_info.wqe_bulk;
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

static inline void mlx5e_cacheflow_put_rx_frag(struct mlx5e_cacheflow_rq *rq,
					       struct page **frag)
{
	if (*frag) {
		cacheflow_page_pool_put_page(rq->page_pool, *frag, -1, true);
		trace_skb_cacheflow_memory_location(page_to_netmem(*frag), NETMEM_LOCATION_POOL);
		*frag = NULL;
	}
}

static inline void mlx5e_cacheflow_free_rx_wqe(struct mlx5e_cacheflow_rq *rq,
					       struct page **wi)
{
	int i;

	for (i = 0; i < rq->wqe.info.num_frags; i++, wi++)
		mlx5e_cacheflow_put_rx_frag(rq, wi);
}

static void mlx5e_cacheflow_free_rx_wqes(struct mlx5e_cacheflow_rq *rq, u16 ix,
					 int wqe_bulk)
{
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	int i;

	for (i = 0; i < wqe_bulk; i++) {
		int j = mlx5_wq_cyc_ctr2ix(wq, ix + i);
		struct page **wi;

		wi = get_frag(rq, j);
		mlx5e_cacheflow_free_rx_wqe(rq, wi);
	}
}

static int mlx5e_cacheflow_alloc_rx_wqe(struct mlx5e_cacheflow_rq *rq,
					struct mlx5e_rx_wqe_cyc *wqe, u16 ix)
{
	struct page **frag = get_frag(rq, ix);
	int i;

	for (i = 0; i < rq->wqe.info.num_frags; i++, frag++) {
		dma_addr_t addr;
		u16 headroom;

		if (unlikely(*frag != NULL)) {
			BUG();
		}

		*frag = cacheflow_page_pool_alloc_pages(
			rq->page_pool, GFP_ATOMIC | __GFP_NOWARN);

		trace_skb_cacheflow_memory_location(page_to_netmem(*frag), NETMEM_LOCATION_RING);

		if (unlikely(*frag == NULL))
			goto free_frags;

		headroom = i == 0 ? rq->buff.headroom : 0;
		addr = page_pool_get_dma_addr(*frag);
		wqe->data[i].addr = cpu_to_be64(addr + headroom);
	}

	return 0;

free_frags:
	while (--i >= 0)
		mlx5e_cacheflow_put_rx_frag(rq, --frag);

	return -ENOMEM;
}

static int mlx5e_cacheflow_alloc_rx_wqes(struct mlx5e_cacheflow_rq *rq, u16 ix,
					 int wqe_bulk)
{
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	int i;

	for (i = 0; i < wqe_bulk; i++) {
		int j = mlx5_wq_cyc_ctr2ix(wq, ix + i);
		struct mlx5e_rx_wqe_cyc *wqe;

		wqe = mlx5_wq_cyc_get_wqe(wq, j);

		if (unlikely(mlx5e_cacheflow_alloc_rx_wqe(rq, wqe, j)))
			break;
	}

	return i;
}

static int mlx5e_cacheflow_refill_rx_wqes(struct mlx5e_cacheflow_rq *rq, u16 ix,
					  int wqe_bulk)
{
	int remaining = wqe_bulk;
	int total_alloc = 0;
	int refill_alloc;
	int refill;

	/* The WQE bulk is split into smaller bulks that are sized
	 * according to the page pool cache refill size to avoid overflowing
	 * the page pool cache due to too many page releases at once.
	 */
	do {
		refill = min_t(u16, rq->wqe.info.refill_unit, remaining);

		mlx5e_cacheflow_free_rx_wqes(rq, ix + total_alloc, refill);
		refill_alloc = mlx5e_cacheflow_alloc_rx_wqes(
			rq, ix + total_alloc, refill);
		if (unlikely(refill_alloc != refill))
			goto err_free;

		total_alloc += refill_alloc;
		remaining -= refill;
	} while (remaining);

	return total_alloc;

err_free:
	mlx5e_cacheflow_free_rx_wqes(rq, ix, total_alloc + refill_alloc);

	return 0;
}

bool mlx5e_cacheflow_post_rx_wqes(struct mlx5e_cacheflow_rq *rq)
{
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	int wqe_bulk, count;
	bool busy = false;
	u16 head;

	if (unlikely(!test_bit(MLX5E_RQ_STATE_ENABLED, &rq->state)))
		return false;

	if (mlx5_wq_cyc_missing(wq) < rq->wqe.info.wqe_bulk)
		return false;

	wqe_bulk = mlx5_wq_cyc_missing(wq);
	head = mlx5_wq_cyc_get_head(wq);

	count = mlx5e_cacheflow_refill_rx_wqes(rq, head, wqe_bulk);

	mlx5_wq_cyc_push_n(wq, count);
	if (unlikely(count != wqe_bulk)) {
		rq->stats->buff_alloc_err++;
		busy = true;
	}

	trace_mlx5e_wqe_post(rq->ix, wqe_bulk);

	/* ensure wqes are visible to device before updating doorbell record */
	dma_wmb();

	mlx5_wq_cyc_update_db_record(wq);

	return busy;
}

static int mlx5e_cacheflow_open_rx_cq(struct mlx5e_cacheflow *c,
				      struct mlx5e_cacheflow_params *cparams)
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

	err = mlx5e_open_cq(c->mdev, moder, &cparams->rq_param.cqp, &ccp,
			    &c->rq.cq);

	return err;
}

static int mlx5e_cacheflow_init_rq(struct mlx5e_cacheflow *c,
				   struct mlx5e_params *params,
				   struct mlx5e_cacheflow_rq *rq)
{
	struct mlx5_core_dev *mdev = c->mdev;
	struct mlx5e_priv *priv = c->priv;

	rq->wq_type = params->rq_wq_type;
	rq->pdev = c->pdev;
	rq->netdev = priv->netdev;
	rq->priv = priv;
	rq->clock = &mdev->clock;
	rq->tstamp = &priv->tstamp;
	rq->mdev = mdev;
	rq->hw_mtu = MLX5E_SW2HW_MTU(params, params->sw_mtu);
	rq->stats = &c->priv->cacheflow_stats.rq;
	rq->ix = 0;
	rq->ptp_cyc2time = mlx5_rq_ts_translator(mdev);

	xdp_rxq_info_unused(&rq->xdp_rxq);
	return 0;
}

int mlx5e_cacheflow_create_rq(struct mlx5e_cacheflow_rq *rq,
			      struct mlx5e_rq_param *param, u16 q_counter)
{
	struct mlx5_core_dev *mdev = rq->mdev;
	u8 ts_format;
	void *in;
	void *rqc;
	void *wq;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_rq_in) +
		sizeof(u64) * rq->wq_ctrl.buf.npages;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	ts_format = mlx5_is_real_time_rq(mdev) ?
			    MLX5_TIMESTAMP_FORMAT_REAL_TIME :
			    MLX5_TIMESTAMP_FORMAT_FREE_RUNNING;
	rqc = MLX5_ADDR_OF(create_rq_in, in, ctx);
	wq = MLX5_ADDR_OF(rqc, rqc, wq);

	memcpy(rqc, param->rqc, sizeof(param->rqc));

	MLX5_SET(rqc, rqc, cqn, rq->cq.mcq.cqn);
	MLX5_SET(rqc, rqc, state, MLX5_RQC_STATE_RST);
	MLX5_SET(rqc, rqc, ts_format, ts_format);
	MLX5_SET(rqc, rqc, counter_set_id, q_counter);
	MLX5_SET(wq, wq, log_wq_pg_sz,
		 rq->wq_ctrl.buf.page_shift - MLX5_ADAPTER_PAGE_SHIFT);
	MLX5_SET64(wq, wq, dbr_addr, rq->wq_ctrl.db.dma);

	mlx5_fill_page_frag_array(&rq->wq_ctrl.buf,
				  (__be64 *)MLX5_ADDR_OF(wq, wq, pas));

	err = mlx5_core_create_rq(mdev, in, inlen, &rq->rqn);

	kvfree(in);

	return err;
}

static int mlx5e_cacheflow_init_wqe_alloc_info(struct mlx5e_cacheflow_rq *rq,
					       int node)
{
	int wq_sz = mlx5_wq_cyc_get_size(&rq->wqe.wq);
	int len = wq_sz << rq->wqe.info.log_num_frags;
	struct page **frags;

	frags = kvzalloc_node(array_size(len, sizeof(*frags)), GFP_KERNEL,
			      node);
	if (!frags)
		return -ENOMEM;

	rq->wqe.frags = frags;

	return 0;
}

static void mlx5e_cacheflow_free_wqe_alloc_info(struct mlx5e_cacheflow_rq *rq)
{
	kvfree(rq->wqe.frags);
}

static int mlx5e_cacheflow_alloc_rq(struct mlx5e_params *params,
				    struct mlx5e_xsk_param *xsk,
				    struct mlx5e_rq_param *rqp, int node,
				    struct mlx5e_cacheflow_rq *rq)
{
	struct mlx5_core_dev *mdev = rq->mdev;
	void *rqc = rqp->rqc;
	void *rqc_wq = MLX5_ADDR_OF(rqc, rqc, wq);
	u32 pool_size;
	int wq_sz;
	int err;
	int i;

	rqp->wq.db_numa_node = node;

	rq->buff.map_dir = DMA_FROM_DEVICE;
	rq->buff.headroom = mlx5e_get_rq_headroom(mdev, params, xsk);
	pool_size = 1 << params->log_rq_mtu_frames;

	rq->mkey_be = cpu_to_be32(mdev->mlx5e_res.hw_objs.mkey);

	err = mlx5_wq_cyc_create(mdev, &rqp->wq, rqc_wq, &rq->wqe.wq,
				 &rq->wq_ctrl);
	if (err)
		goto err_rq_xdp_prog;

	rq->wqe.wq.db = &rq->wqe.wq.db[MLX5_RCV_DBR];

	wq_sz = mlx5_wq_cyc_get_size(&rq->wqe.wq);

	rq->wqe.info = rqp->frags_info;
	rq->buff.frame0_sz = rq->wqe.info.arr[0].frag_stride;

	err = mlx5e_cacheflow_init_wqe_alloc_info(rq, node);
	if (err)
		goto err_rq_wq_destroy;

	/* Create a page_pool and register it with rxq */
	struct cacheflow_page_pool_params pp_params = { 0 };

	pp_params.order = order_base_2(max(MLX5E_SW2HW_MTU(params, params->sw_mtu), PAGE_SIZE)) - PAGE_SHIFT;
	pp_params.pool_size = 4096;
	pp_params.nid = node;
	pp_params.dev = rq->pdev;
	pp_params.napi = rq->cq.napi;
	pp_params.netdev = rq->netdev;
	pp_params.dma_dir = rq->buff.map_dir;
	pp_params.max_len = PAGE_SIZE;

	/* page_pool can be used even when there is no rq->xdp_prog,
	 * given page_pool does not handle DMA mapping there is no
	 * required state to clear. And page_pool gracefully handle
	 * elevated refcnt.
	 */
	rq->page_pool = cacheflow_page_pool_create(&pp_params);
	if (IS_ERR(rq->page_pool)) {
		err = PTR_ERR(rq->page_pool);
		rq->page_pool = NULL;
		goto err_free_by_rq_type;
	}
	if (xdp_rxq_info_is_reg(&rq->xdp_rxq))
		err = xdp_rxq_info_reg_mem_model(
			&rq->xdp_rxq, MEM_TYPE_PAGE_POOL, rq->page_pool);

	if (err)
		goto err_destroy_page_pool;

	for (i = 0; i < wq_sz; i++) {
		struct mlx5e_rx_wqe_cyc *wqe =
			mlx5_wq_cyc_get_wqe(&rq->wqe.wq, i);
		int f;

		for (f = 0; f < rq->wqe.info.num_frags; f++) {
			u32 frag_size = rq->wqe.info.arr[f].frag_size |
					MLX5_HW_START_PADDING;

			wqe->data[f].byte_count = cpu_to_be32(frag_size);
			wqe->data[f].lkey = rq->mkey_be;
		}
		/* check if num_frags is not a pow of two */
		if (rq->wqe.info.num_frags <
		    (1 << rq->wqe.info.log_num_frags)) {
			wqe->data[f].byte_count = 0;
			wqe->data[f].lkey = params->terminate_lkey_be;
			wqe->data[f].addr = 0;
		}
	}

	return 0;

err_destroy_page_pool:
	cacheflow_page_pool_destroy(rq->page_pool);
err_free_by_rq_type:
	mlx5e_cacheflow_free_wqe_alloc_info(rq);
err_rq_wq_destroy:
	mlx5_wq_destroy(&rq->wq_ctrl);
err_rq_xdp_prog:
	if (params->xdp_prog)
		bpf_prog_put(params->xdp_prog);

	return err;
}

static int mlx5_core_set_delay_drop(struct mlx5_core_dev *dev, u32 timeout_usec)
{
	u32 in[MLX5_ST_SZ_DW(set_delay_drop_params_in)] = {};

	MLX5_SET(set_delay_drop_params_in, in, opcode,
		 MLX5_CMD_OP_SET_DELAY_DROP_PARAMS);
	MLX5_SET(set_delay_drop_params_in, in, delay_drop_timeout,
		 timeout_usec / 100);
	return mlx5_cmd_exec_in(dev, set_delay_drop_params, in);
}

static int mlx5e_set_delay_drop(struct mlx5e_priv *priv,
				struct mlx5e_params *params)
{
	struct mlx5e_delay_drop *delay_drop = &priv->delay_drop;
	int err = 0;

	if (!MLX5E_GET_PFLAG(params, MLX5E_PFLAG_DROPLESS_RQ)) {
		delay_drop->activate = false;
		return 0;
	}

	mutex_lock(&delay_drop->lock);
	if (delay_drop->activate)
		goto out;

	err = mlx5_core_set_delay_drop(priv->mdev, delay_drop->usec_timeout);
	if (err)
		goto out;

	delay_drop->activate = true;
out:
	mutex_unlock(&delay_drop->lock);
	return err;
}

static int mlx5e_cacheflow_modify_rq_state(struct mlx5e_cacheflow_rq *rq,
					   int curr_state, int next_state)
{
	struct mlx5_core_dev *mdev = rq->mdev;

	void *in;
	void *rqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_rq_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	if (curr_state == MLX5_RQC_STATE_RST &&
	    next_state == MLX5_RQC_STATE_RDY)
		mlx5_wq_cyc_reset(&rq->wqe.wq);

	rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);

	MLX5_SET(modify_rq_in, in, rq_state, curr_state);
	MLX5_SET(rqc, rqc, state, next_state);

	err = mlx5_core_modify_rq(mdev, rq->rqn, in);

	kvfree(in);

	return err;
}

static void mlx5e_cacheflow_free_rq(struct mlx5e_cacheflow_rq *rq)
{
	kvfree(rq->dim);
	cacheflow_page_pool_destroy(rq->page_pool);

	mlx5e_cacheflow_free_wqe_alloc_info(rq);

	mlx5_wq_destroy(&rq->wq_ctrl);
}

void mlx5e_cacheflow_destroy_rq(struct mlx5e_cacheflow_rq *rq)
{
	mlx5_core_destroy_rq(rq->mdev, rq->rqn);
}

static void mlx5e_cacheflow_dealloc_rx_wqe(struct mlx5e_cacheflow_rq *rq,
					   u16 ix)
{
	struct page **wi = get_frag(rq, ix);

	mlx5e_cacheflow_free_rx_wqe(rq, wi);
}

static void mlx5e_cacheflow_free_rx_descs(struct mlx5e_cacheflow_rq *rq)
{
	u16 wqe_ix;

	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	u16 missing = mlx5_wq_cyc_missing(wq);
	u16 head = mlx5_wq_cyc_get_head(wq);

	while (!mlx5_wq_cyc_is_empty(wq)) {
		wqe_ix = mlx5_wq_cyc_get_tail(wq);
		mlx5e_cacheflow_dealloc_rx_wqe(rq, wqe_ix);
		mlx5_wq_cyc_pop(wq);
	}
	/* Missing slots might also contain unreleased pages due to
	 * deferred release.
	 */
	while (missing--) {
		wqe_ix = mlx5_wq_cyc_ctr2ix(wq, head++);
		mlx5e_cacheflow_dealloc_rx_wqe(rq, wqe_ix);
	}
}

static int mlx5e_cacheflow_open_rq(struct mlx5e_params *params,
				   struct mlx5e_rq_param *param,
				   struct mlx5e_xsk_param *xsk, int node,
				   u16 q_counter, struct mlx5e_cacheflow_rq *rq)
{
	struct mlx5_core_dev *mdev = rq->mdev;
	int err;

	err = mlx5e_cacheflow_alloc_rq(params, xsk, param, node, rq);
	if (err)
		return err;

	err = mlx5e_cacheflow_create_rq(rq, param, q_counter);
	if (err)
		goto err_free_rq;

	err = mlx5e_set_delay_drop(rq->priv, params);
	if (err)
		mlx5_core_warn(mdev, "Failed to enable delay drop err=%d\n",
			       err);

	err = mlx5e_cacheflow_modify_rq_state(rq, MLX5_RQC_STATE_RST,
					      MLX5_RQC_STATE_RDY);
	if (err)
		goto err_destroy_rq;

	if (MLX5_CAP_ETH(mdev, cqe_checksum_full))
		__set_bit(MLX5E_RQ_STATE_CSUM_FULL, &rq->state);

	if (rq->channel)
		rq->channel->rx_cq_moder = params->rx_cq_moderation;

	return 0;

err_destroy_rq:
	mlx5e_cacheflow_destroy_rq(rq);
err_free_rq:
	mlx5e_cacheflow_free_rq(rq);

	return err;
}

void mlx5e_cacheflow_activate_rq(struct mlx5e_cacheflow_rq *rq)
{
	set_bit(MLX5E_RQ_STATE_ENABLED, &rq->state);
}

void mlx5e_cacheflow_deactivate_rq(struct mlx5e_cacheflow_rq *rq)
{
	clear_bit(MLX5E_RQ_STATE_ENABLED, &rq->state);
	synchronize_net(); /* Sync with NAPI to prevent mlx5e_post_rx_wqes. */
}

void mlx5e_cacheflow_close_rq(struct mlx5e_cacheflow_rq *rq)
{
	if (rq->dim)
		cancel_work_sync(&rq->dim->work);

	mlx5e_cacheflow_destroy_rq(rq);
	mlx5e_cacheflow_free_rx_descs(rq);
	mlx5e_cacheflow_free_rq(rq);
}

static int mlx5e_cacheflow_open_rxq_rq(struct mlx5e_cacheflow *c,
				       struct mlx5e_params *params,
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

	return mlx5e_cacheflow_open_rq(params, rq_param, NULL, node, q_counter,
				       &c->rq);
}

static int mlx5e_cacheflow_open_queues(struct mlx5e_cacheflow *c,
				       struct mlx5e_cacheflow_params *cparams)
{
	int err;

	err = mlx5e_cacheflow_open_rx_cq(c, cparams);
	if (err)
		return err;

	err = mlx5e_cacheflow_open_rxq_rq(c, &cparams->params,
					  &cparams->rq_param);
	if (err)
		goto close_rx_cq;

	return 0;

close_rx_cq:
	mlx5e_close_cq(&c->rq.cq);

	return err;
}

static void mlx5e_cacheflow_close_queues(struct mlx5e_cacheflow *c)
{
	mlx5e_cacheflow_close_rq(&c->rq);
	mlx5e_close_cq(&c->rq.cq);
}

static void mlx5e_cacheflow_print_params(struct mlx5e_cacheflow_params *cparams)
{
	struct mlx5e_params *params = &cparams->params;
	struct mlx5e_rq_param *rq_param = &cparams->rq_param;

	pr_info("cacheflow channel params:\n");
	pr_info("  mlx5e_params: log_sq_size=%u, rq_wq_type=%u, log_rq_mtu_frames=%u, num_channels=%u\n",
		params->log_sq_size, params->rq_wq_type,
		params->log_rq_mtu_frames, params->num_channels);
	pr_info("  mqprio: mode=%u, num_tc=%u\n", params->mqprio.mode,
		params->mqprio.num_tc);
	pr_info("  cqe_compress_def=%d, vlan_strip_disable=%d, scatter_fcs=%d\n",
		params->rx_cqe_compress_def, params->vlan_strip_disable,
		params->scatter_fcs_en);
	pr_info("  dim: rx_en=%d, tx_en=%d, rx_use_cqe=%d, tx_use_cqe=%d\n",
		params->rx_dim_enabled, params->tx_dim_enabled,
		params->rx_moder_use_cqe_mode, params->tx_moder_use_cqe_mode);
	pr_info("  packet_merge: type=%d, timeout=%u, shampo(match_type=%u, align_gran=%u)\n",
		params->packet_merge.type, params->packet_merge.timeout,
		params->packet_merge.shampo.match_criteria_type,
		params->packet_merge.shampo.alignment_granularity);
	pr_info("  tx_min_inline=%u, pflags=0x%x, sw_mtu=%u, hard_mtu=%d, ptp_rx=%d, lkey=0x%x\n",
		params->tx_min_inline_mode, params->pflags, params->sw_mtu,
		params->hard_mtu, params->ptp_rx,
		be32_to_cpu(params->terminate_lkey_be));

	pr_info("  mlx5e_rq_param:\n");
	pr_info("    cqp: eq_ix=%u, cq_period_mode=%u, cqc_size=%zu\n",
		rq_param->cqp.eq_ix, rq_param->cqp.cq_period_mode,
		sizeof(rq_param->cqp.cqc));
	pr_info("      cqp.wq: buf_numa_node=%d, db_numa_node=%d\n",
		rq_param->cqp.wq.buf_numa_node, rq_param->cqp.wq.db_numa_node);
	pr_info("    rqc_size: %zu\n", sizeof(rq_param->rqc));
	pr_info("    wq: buf_numa_node=%d, db_numa_node=%d\n",
		rq_param->wq.buf_numa_node, rq_param->wq.db_numa_node);
	pr_info("    frags_info: num_frags=%u, log_num_frags=%u, wqe_bulk=%u, refill_unit=%u, wqe_index_mask=0x%x\n",
		rq_param->frags_info.num_frags,
		rq_param->frags_info.log_num_frags,
		rq_param->frags_info.wqe_bulk, rq_param->frags_info.refill_unit,
		rq_param->frags_info.wqe_index_mask);
	for (int i = 0; i < rq_param->frags_info.num_frags; i++) {
		pr_info("      frags_info.arr[%d]: frag_size=%d, frag_stride=%d\n",
			i, rq_param->frags_info.arr[i].frag_size,
			rq_param->frags_info.arr[i].frag_stride);
	}
}

static void cacheflow_raise_softirq(void *data)
{
	struct mlx5e_cacheflow_th *th = data;

	napi_schedule_irqoff(&th->napi);
}

static int mlx5e_cacheflow_th_init(struct mlx5e_cacheflow_th *th, int cpu,
				   struct mlx5e_cacheflow *cacheflow,
				   struct mlx5e_cacheflow_rq *rq)
{
	th->rq = rq;
	th->cpu = cpu;
	th->cacheflow = cacheflow;
	th->ipi_scheduled = 0;
	th->last_scheduled_time = 0;

	th->inserted = 0;
	th->missed = 0;

	INIT_CSD(&th->csd, cacheflow_raise_softirq, th);
	spin_lock_init(&th->cqe_fifo_lock);
	th->cqe_ring = item_ring_create(
		8192, sizeof(struct mlx5e_cacheflow_cqe), GFP_KERNEL);

	mlx5e_cacheflow_th_debugfs_init(th);

	return 0;
}

int mlx5e_cacheflow_open(struct mlx5e_priv *priv, struct mlx5e_params *params,
			 u8 lag_port, struct mlx5e_cacheflow **cc)
{
	struct net_device *netdev = priv->netdev;
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_cacheflow_params *cparams;
	struct mlx5e_cacheflow *c;
	struct mlx5e_cacheflow_th *th;
	struct mlx5e_cacheflow_rq_tracker *rq_tracker = NULL;
	int err, cpu;

	c = kvzalloc_node(sizeof(*c), GFP_KERNEL,
			  dev_to_node(mlx5_core_dma_dev(mdev)));
	cparams = kvzalloc(sizeof(*cparams), GFP_KERNEL);
	th = kvcalloc(num_possible_cpus(), sizeof(*th), GFP_KERNEL);

	if (!c || !cparams || !th) {
		err = -ENOMEM;
		goto err_free;
	}

	if (cacheflow_rq_tracker) {
		rq_tracker = mlx5e_cacheflow_rq_tracker_create(
			1 << (params->log_rq_mtu_frames + 1));
		if (!rq_tracker) {
			err = -ENOMEM;
			goto err_free;
		}
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
	c->rq_tracker = rq_tracker;

	mlx5e_cacheflow_debugfs_init(c);

	mlx5e_cacheflow_build_params(c, cparams, params);
	mlx5e_cacheflow_print_params(cparams);

	netif_cacheflow_napi_add_weight(netdev, &c->napi,
					mlx5e_cacheflow_bh_napi_poll, 16,
					get_cacheflow_steer_core());
	pr_info("cacheflow: add NAPI %d (kthread) on core %d, res: %s\n",
		c->napi.napi_id, get_cacheflow_steer_core(),
		test_bit(NAPI_STATE_CACHEFLOW, &c->napi.state) ? "succeed" :
								 "fail");

	err = mlx5e_cacheflow_open_queues(c, cparams);
	if (unlikely(err))
		goto err_napi_del;

	c->th_array = th;
	cpumask_clear(&c->notify_cpu_set);
	for_each_possible_cpu(cpu) {
		mlx5e_cacheflow_th_init(&c->th_array[cpu], cpu, c, &c->rq);
		netif_napi_add(netdev, &c->th_array[cpu].napi,
			       mlx5e_cacheflow_th_napi_poll);
	}

	priv->cacheflow_opened = true;

	*cc = c;

	kvfree(cparams);
	return 0;

err_napi_del:
	netif_napi_del(&c->napi);
err_free:
	kvfree(th);
	kvfree(cparams);
	kvfree(c);
	kvfree(rq_tracker);
	return err;
}

static void mlx5e_cacheflow_th_destroy(struct mlx5e_cacheflow_th *th)
{
	netif_napi_del(&th->napi);
	item_ring_destroy(th->cqe_ring);
	mlx5e_cacheflow_th_debugfs_destroy(th);
}

void mlx5e_cacheflow_close(struct mlx5e_cacheflow *c)
{
	int cpu;

	mlx5e_cacheflow_close_queues(c);
	netif_napi_del(&c->napi);

	for_each_possible_cpu(cpu) {
		mlx5e_cacheflow_th_destroy(&c->th_array[cpu]);
	}

	if (c->rq_tracker)
		mlx5e_cacheflow_rq_tracker_destroy(c->rq_tracker);

	mlx5e_cacheflow_debugfs_destroy(c);

	kvfree(c->th_array);
	kvfree(c);
}

void mlx5e_cacheflow_activate_channel(struct mlx5e_cacheflow *c)
{
	int cpu;

	napi_enable(&c->napi);

	for_each_possible_cpu(cpu) {
		napi_enable(&c->th_array[cpu].napi);
	}

	mlx5e_cacheflow_activate_rq(&c->rq);

	mlx5e_trigger_napi_sched(&c->napi);

	for_each_possible_cpu(cpu) {
		smp_call_function_single_async(cpu, &c->th_array[cpu].csd);
	}
}

void mlx5e_cacheflow_deactivate_channel(struct mlx5e_cacheflow *c)
{
	int cpu;

	mlx5e_cacheflow_deactivate_rq(&c->rq);
	napi_disable(&c->napi);

	for_each_possible_cpu(cpu) {
		napi_disable(&c->th_array[cpu].napi);
	}
}

static inline void mlx5e_skb_set_hash(struct mlx5_cqe64 *cqe,
				      struct sk_buff *skb)
{
	u8 cht = cqe->rss_hash_type;
	int ht = (cht & CQE_RSS_HTYPE_L4) ? PKT_HASH_TYPE_L4 :
		 (cht & CQE_RSS_HTYPE_IP) ? PKT_HASH_TYPE_L3 :
					    PKT_HASH_TYPE_NONE;
	skb_set_hash(skb, be32_to_cpu(cqe->rss_hash_result), ht);
}
