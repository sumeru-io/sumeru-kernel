// SPDX-License-Identifier: GPL-2.0
/*
 * cacheflow.c
 *	Author:	Minhu Wang <minhuw@acm.org>
 */

#include <linux/cache.h>
#include <linux/jump_label.h>

u8 cacheflow_mark_enable __read_mostly;
u8 cacheflow_track_enable __read_mostly;

struct static_key_false cacheflow_steer_enable __read_mostly;
EXPORT_SYMBOL(cacheflow_steer_enable);

int cacheflow_steer_core __read_mostly;
int cacheflow_buffer_size __read_mostly = 4096;
int cacheflow_thresh __read_mostly = 2048;
int cacheflow_ipi_packet_thresh __read_mostly = 16;
int cacheflow_ipi_usec_thresh __read_mostly = 128;
int cacheflow_elephant_flow_thresh __read_mostly = 256;
