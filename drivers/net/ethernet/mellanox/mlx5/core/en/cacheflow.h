#ifndef __MLX5_EN_CACHEFLOW_H__
#define __MLX5_EN_CACHEFLOW_H__

#include "en.h"

struct mlx5e_cacheflow {
	struct mlx5e_rq			rq;

	struct napi_struct         	napi;
	struct device             	*pdev;
	struct net_device         	*netdev;
	__be32                     	mkey_be;
	u8                         	num_tc;
	u8                         	lag_port;

	struct mlx5e_ch_stats     	*stats;

	/* control */
	struct mlx5e_priv         	*priv;
	struct mlx5_core_dev      	*mdev;
	struct hwtstamp_config    	*tstamp;
	int                        	cpu;
};

int mlx5e_cacheflow_open(struct mlx5e_priv *priv, struct mlx5e_params *params,
			 u8 lag_port, struct mlx5e_cacheflow **c);

void mlx5e_cacheflow_close(struct mlx5e_cacheflow *c);
void mlx5e_cacheflow_activate_channel(struct mlx5e_cacheflow *c);
void mlx5e_cacheflow_deactivate_channel(struct mlx5e_cacheflow *c);

#endif
