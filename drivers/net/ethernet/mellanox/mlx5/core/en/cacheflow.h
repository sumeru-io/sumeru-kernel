#ifndef __MLX5_EN_CACHEFLOW_H__
#define __MLX5_EN_CACHEFLOW_H__

#include <linux/kfifo.h>
#include "en.h"

#define CACHEFLOW_CHANNEL_SIZE 128

struct mlx5e_cacheflow {
	struct mlx5e_rq			rq;

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
	struct napi_struct	napi;
	struct mlx5e_rq		*rq;
	int 			cpu;

	call_single_data_t	csd ____cacheline_aligned_in_smp;
	int			ipi_scheduled;

	DECLARE_KFIFO(cqe_fifo, struct mlx5e_cacheflow_cqe, CACHEFLOW_CHANNEL_SIZE) ____cacheline_aligned_in_smp;
};

int mlx5e_cacheflow_open(struct mlx5e_priv *priv, struct mlx5e_params *params,
			 u8 lag_port, struct mlx5e_cacheflow **c);

void mlx5e_cacheflow_close(struct mlx5e_cacheflow *c);
void mlx5e_cacheflow_activate_channel(struct mlx5e_cacheflow *c);
void mlx5e_cacheflow_deactivate_channel(struct mlx5e_cacheflow *c);

#endif
