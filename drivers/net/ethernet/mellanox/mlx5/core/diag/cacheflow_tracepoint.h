/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mlx5

#if !defined(_MLX5_CACHEFLOW_TP_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _MLX5_CACHEFLOW_TP_H_

#include <linux/tracepoint.h>

TRACE_EVENT(mlx5e_mpwqe_post,

	TP_PROTO(int ix, u32 umr_completed, u32 umr_in_progress, u32 umr_missing),

	TP_ARGS(ix, umr_completed, umr_in_progress, umr_missing),

	TP_STRUCT__entry(
		__field(int, ix)
		__field(u32, umr_completed)
		__field(u32, umr_in_progress)
		__field(u32, umr_missing)
	),

	TP_fast_assign(
		__entry->ix = ix;
		__entry->umr_completed = umr_completed;
		__entry->umr_in_progress = umr_in_progress;
		__entry->umr_missing = umr_missing;
	),

	TP_printk("ix %d, umr_completed %u, umr_in_progress %u, umr_missing %u",
		  __entry->ix,
		  __entry->umr_completed,
		  __entry->umr_in_progress,
		  __entry->umr_missing)
);

TRACE_EVENT(mlx5e_wqe_post,

	TP_PROTO(int ix, u32 wqe_bulk),

	TP_ARGS(ix, wqe_bulk),

	TP_STRUCT__entry(
		__field(int, ix)
		__field(u32, wqe_bulk)
	),

	TP_fast_assign(
		__entry->ix = ix;
		__entry->wqe_bulk = wqe_bulk;
	),

	TP_printk("ix %d, wqe_bulk %u",
		  __entry->ix,
		  __entry->wqe_bulk)
);

#endif /* _MLX5_CACHEFLOW_TP_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ./diag
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE cacheflow_tracepoint
#include <trace/define_trace.h>
