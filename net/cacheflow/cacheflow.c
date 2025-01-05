
/* SPDX-License-Identifier: GPL-2.0
 * 
 * cacheflow.c
 * 	Author:	Minhu Wang <minhuw@acm.org>
 */

#include <asm/cache.h>

u8 cacheflow_enable __read_mostly = 0;
int cacheflow_buffer_size __read_mostly = 2048;