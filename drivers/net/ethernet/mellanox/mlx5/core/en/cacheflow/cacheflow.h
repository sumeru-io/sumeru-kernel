#ifndef __MLX5_EN_CACHEFLOW_H__
#define __MLX5_EN_CACHEFLOW_H__

#include <linux/kfifo.h>
#include "en.h"

#define CACHEFLOW_CHANNEL_SIZE 128

struct mlx5e_cacheflow_xdp_buff {
	struct xdp_buff xdp;
	struct mlx5_cqe64 *cqe;
	struct mlx5e_cacheflow_rq *rq;
};

struct mlx5e_cacheflow_rq {
	struct {
		struct mlx5_wq_cyc          		wq;
		struct page**                           frags;
		struct mlx5e_rq_frags_info  		info;
	} wqe;
	struct {
		u16            headroom;
		u32            frame0_sz;
		u8             map_dir;   /* dma map direction */
	} buff;
	struct device         *pdev;
	struct net_device     *netdev;
	struct mlx5e_rq_stats *stats;
	struct mlx5e_cq        cq;
	struct mlx5e_cq_decomp cqd;
	struct hwtstamp_config *tstamp;
	struct mlx5_clock      *clock;
	struct mlx5e_icosq    *icosq;
	struct mlx5e_priv     *priv;

	struct mlx5e_hw_gro_data *hw_gro_data;

	unsigned long          state;
	int                    ix;
	unsigned int           hw_mtu;

	struct dim            *dim; /* Dynamic Interrupt Moderation */

	/* XDP */
	DECLARE_BITMAP(flags, 8);
	struct cacheflow_page_pool      *page_pool;

	/* AF_XDP zero-copy */
	struct xsk_buff_pool  *xsk_pool;

	/* control */
	struct mlx5_wq_ctrl    wq_ctrl;
	__be32                 mkey_be;
	u8                     wq_type;
	u32                    rqn;
	struct mlx5_core_dev  *mdev;
	struct mlx5e_channel  *channel;
	struct mlx5e_dma_info  wqe_overflow;

	/* XDP read-mostly */
	struct xdp_rxq_info    xdp_rxq;
	cqe_ts_to_ns           ptp_cyc2time;
};

struct mlx5e_cacheflow {
	struct mlx5e_cacheflow_rq	rq;

	struct napi_struct         	napi;
	struct device             	*pdev;
	struct net_device         	*netdev;
	__be32                     	mkey_be;
	u8                         	num_tc;
	u8                         	lag_port;

	struct mlx5e_ch_stats     	*stats;
	struct mlx5e_cacheflow_th  	*th_array;
	cpumask_t 			notify_cpu_set;
	/* control */
	struct mlx5e_priv         	*priv;
	struct mlx5_core_dev      	*mdev;
	struct hwtstamp_config    	*tstamp;
	int                        	cpu;
};

enum mlx5e_cacheflow_cqe_owner {
	MLX5E_CACHEFLOW_CQ_OWNER_BH,
	MLX5E_CACHEFLOW_CQ_OWNER_TH,
};

struct mlx5e_cacheflow_wqe_frags_info {
	struct page *page[MLX5E_MAX_RX_FRAGS];
	u32 offset[MLX5E_MAX_RX_FRAGS];
	u8 flags[MLX5E_MAX_RX_FRAGS];
	u8 num_frags;
};

struct mlx5e_cacheflow_cqe {
	struct mlx5_cqe64 cqe;
	int owner;
	struct page *page[MLX5E_MAX_RX_FRAGS];
} ____cacheline_aligned;

struct mlx5e_cacheflow_th {
	struct napi_struct		napi;
	struct mlx5e_cacheflow_rq	*rq;
	int				cpu;

	call_single_data_t		csd ____cacheline_aligned_in_smp;
	int				ipi_scheduled;

	DECLARE_KFIFO(cqe_fifo, struct mlx5e_cacheflow_cqe, CACHEFLOW_CHANNEL_SIZE) ____cacheline_aligned_in_smp;
};

int mlx5e_cacheflow_open(struct mlx5e_priv *priv, struct mlx5e_params *params,
			 u8 lag_port, struct mlx5e_cacheflow **c);


int mlx5e_cacheflow_th_napi_poll(struct napi_struct *napi, int budget);
int mlx5e_cacheflow_bh_napi_poll(struct napi_struct *napi, int budget);

void mlx5e_cacheflow_close(struct mlx5e_cacheflow *c);
void mlx5e_cacheflow_activate_channel(struct mlx5e_cacheflow *c);
void mlx5e_cacheflow_deactivate_channel(struct mlx5e_cacheflow *c);

bool mlx5e_cacheflow_post_rx_wqes(struct mlx5e_cacheflow_rq *rq);
void mlx5e_cacheflow_close_rq(struct mlx5e_cacheflow_rq *rq);
int mlx5e_cacheflow_create_rq(struct mlx5e_cacheflow_rq *rq, struct mlx5e_rq_param *param, u16 q_counter);
void mlx5e_cacheflow_destroy_rq(struct mlx5e_cacheflow_rq *rq);

int mlx5e_cacheflow_flush_rq(struct mlx5e_cacheflow_rq *rq, int curr_state);
void mlx5e_cacheflow_activate_rq(struct mlx5e_cacheflow_rq *rq);
void mlx5e_cacheflow_deactivate_rq(struct mlx5e_cacheflow_rq *rq);

#endif
