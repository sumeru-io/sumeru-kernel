/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cacheflow

#if !defined(_TRACE_CACHEFLOW_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_CACHEFLOW_H

#include <linux/tracepoint.h>
#include <linux/netdevice.h>

TRACE_EVENT(cacheflow_napi_poll,

	TP_PROTO(struct net_device *dev, int core, int work_done),
	TP_ARGS(dev, core, work_done),

	TP_STRUCT__entry(
		__array(char, dev_name, IFNAMSIZ)
		__field(int, core)
		__field(int, work_done)
	),

	TP_fast_assign(
		strscpy(__entry->dev_name, dev ? dev->name : "unknown", IFNAMSIZ);
		__entry->core = core;
		__entry->work_done = work_done;
	),

	TP_printk("dev=%s core=%d work_done=%d", __entry->dev_name, __entry->core, __entry->work_done)
);
#endif /* _TRACE_CACHEFLOW_H */

/* This part must be outside protection */
#include <trace/define_trace.h>