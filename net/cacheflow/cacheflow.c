
/* SPDX-License-Identifier: GPL-2.0
 * 
 * cacheflow.c
 * 	Author:	Minhu Wang <minhuw@acm.org>
 */

#include <asm/cache.h>

u8 cacheflow_mark_enable __read_mostly = 0;
u8 cacheflow_track_enable __read_mostly = 0;
int cacheflow_buffer_size __read_mostly = 4096;
int cacheflow_thresh __read_mostly = 2048;
