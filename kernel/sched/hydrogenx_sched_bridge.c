// SPDX-License-Identifier: GPL-2.0
/*
 * HydrogenX SchedTune/UCLAMP compatibility bridge.
 *
 * This file intentionally lives outside tune_plus.c.  It avoids fragile patch
 * offsets in vendor scheduler files while still providing strong definitions
 * for the weak hooks used by hydrogenx_boost.c.
 */

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/types.h>

#if defined(CONFIG_SCHED_TUNE)
extern int boost_write_for_perf_idx(int idx, int boost_value);
extern int prefer_idle_for_perf_idx(int idx, int prefer_idle);
#endif

#if defined(CONFIG_UCLAMP_TASK_GROUP) && defined(CONFIG_SCHED_TUNE)
extern int uclamp_min_pct_for_perf_idx(int idx, int pct);
#endif

#define HX_STUNE_ROOT        0
#define HX_STUNE_FOREGROUND  1
#define HX_STUNE_TOPAPP      3

static int hx_clamp_0_100(int val)
{
	if (val < 0)
		return 0;
	if (val > 100)
		return 100;
	return val;
}

void schedtune_hydrogenx_set_boost(int boost)
{
#if defined(CONFIG_SCHED_TUNE)
	int val = hx_clamp_0_100(boost);
	int root_val = val / 2;
	int prefer_idle = val > 0;

	/* Best effort: these helpers return errors if a group does not exist. */
	boost_write_for_perf_idx(HX_STUNE_ROOT, root_val);
	boost_write_for_perf_idx(HX_STUNE_FOREGROUND, val);
	boost_write_for_perf_idx(HX_STUNE_TOPAPP, val);
	prefer_idle_for_perf_idx(HX_STUNE_FOREGROUND, prefer_idle);
	prefer_idle_for_perf_idx(HX_STUNE_TOPAPP, prefer_idle);
#else
	(void)boost;
#endif
}

void schedtune_hydrogenx_set_uclamp_min(unsigned int pct)
{
#if defined(CONFIG_UCLAMP_TASK_GROUP) && defined(CONFIG_SCHED_TUNE)
	int val = hx_clamp_0_100((int)pct);
	int root_val = val / 2;

	uclamp_min_pct_for_perf_idx(HX_STUNE_ROOT, root_val);
	uclamp_min_pct_for_perf_idx(HX_STUNE_FOREGROUND, val);
	uclamp_min_pct_for_perf_idx(HX_STUNE_TOPAPP, val);
#else
	(void)pct;
#endif
}
