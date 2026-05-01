
// SPDX-License-Identifier: GPL-2.0
/*
 * HydrogenX schedutil governor for Xiaomi pissarro / MT6877 trees.
 *
 * This is intentionally NOT a dbs/ondemand governor.  It is a schedutil fork:
 * frequency updates are driven by cpufreq_update_util() scheduler callbacks,
 * so it keeps the important schedutil properties:
 *   - PELT/WALT scheduler utilization as the main signal;
 *   - RT/deadline fast max-frequency behavior;
 *   - IO-wait boost integration;
 *   - fast-switch support when the cpufreq driver allows it.
 *
 * HydrogenX additions are kept as floors/clamps around the schedutil result:
 *   - input/frame/game/IO/GPU/DDR scene floors from /sys/kernel/hydrogenx;
 *   - tunable hispeed, game and frame minimum frequencies;
 *   - slower down ramp and optional powersave bias;
 *   - safe hot-path boost-state reads with no sleeping locks.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/hydrogenx_boost.h>
#include <linux/irq_work.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sched/cpufreq.h>
#include <linux/tick.h>
#include <linux/limits.h>

#include "sched.h"
#include "tune.h"
#include "cpufreq_schedutil.h"

#define HXSGOV_DEFAULT_UP_RATE_US        1500U
#define HXSGOV_DEFAULT_DOWN_RATE_US      10000U
#define HXSGOV_DEFAULT_DOWN_HOLD_US      16000U
#define HXSGOV_DEFAULT_HISPEED_LOAD      72U
#define HXSGOV_DEFAULT_BOOST_FLOOR_PCT   76U
#define HXSGOV_DEFAULT_GAME_FLOOR_PCT    62U
#define HXSGOV_DEFAULT_FRAME_FLOOR_PCT   70U
#define HXSGOV_MAX_PCT                   100U
#define HXSGOV_MAX_BIAS                  100U

static struct cpufreq_governor hydrogenx_schedutil_gov;
extern unsigned long boosted_cpu_util(int cpu);
extern void (*cpufreq_notifier_fp)(int cluster_id, unsigned long freq);

struct hsgov_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	unsigned int down_hold_us;
	unsigned int hispeed_load;
	unsigned int hispeed_freq;
	unsigned int floor_pct;
	unsigned int game_min_freq;
	unsigned int frame_min_freq;
	unsigned int boost_floor_pct;
	unsigned int powersave_bias;
	unsigned int aggressive_iowait;
	unsigned int rt_max_boost;
	unsigned int debug;
};

struct hsgov_policy {
	struct cpufreq_policy *policy;
	struct hsgov_tunables *tunables;
	struct list_head tunables_hook;
	raw_spinlock_t update_lock;
	u64 last_freq_update_time;
	s64 min_rate_limit_ns;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;
	s64 down_hold_ns;
	unsigned int next_freq;
	unsigned int cached_raw_freq;
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;
	bool need_freq_update;
};

struct hsgov_cpu {
	struct update_util_data update_util;
	struct hsgov_policy *sg_policy;
	unsigned int cpu;
	bool iowait_boost_pending;
	unsigned int iowait_boost;
	unsigned int iowait_boost_max;
	u64 last_update;
	unsigned long util;
	unsigned long max;
	unsigned int flags;
	unsigned long min_boost;
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct hsgov_cpu, hsgov_cpu);
static struct hsgov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);
static DEFINE_MUTEX(min_rate_lock);

static inline struct hsgov_tunables *to_hsgov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct hsgov_tunables, attr_set);
}

static unsigned int hsgov_clamp_pct(unsigned int val)
{
	return min(val, HXSGOV_MAX_PCT);
}

static unsigned int hsgov_freq_from_pct(struct cpufreq_policy *policy, unsigned int pct)
{
	unsigned int freq;

	pct = hsgov_clamp_pct(pct);
	if (!pct)
		return policy->min;

	freq = policy->cpuinfo.max_freq * pct / 100U;
	return clamp_val(freq, policy->min, policy->max);
}

static unsigned int hsgov_resolve_freq(struct cpufreq_policy *policy, unsigned int freq)
{
	freq = clamp_val(freq, policy->min, policy->max);
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
	return mt_cpufreq_find_close_freq(arch_get_cluster_id(policy->cpu), freq);
#else
	return cpufreq_driver_resolve_freq(policy, freq);
#endif
}

static void hsgov_update_min_rate_limit_ns(struct hsgov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					     sg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static bool hsgov_should_update_freq(struct hsgov_policy *sg_policy, u64 time)
{
	s64 delta_ns;
	struct cpufreq_policy *policy = sg_policy->policy;

	if (policy->governor != &hydrogenx_schedutil_gov || !policy->governor_data)
		return false;

	if (policy->fast_switch_enabled && !cpufreq_can_do_remote_dvfs(policy))
		return false;

	if (sg_policy->work_in_progress)
		return false;

	if (unlikely(sg_policy->need_freq_update)) {
		sg_policy->need_freq_update = false;
		sg_policy->next_freq = UINT_MAX;
		return true;
	}

	delta_ns = time - sg_policy->last_freq_update_time;
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static bool hsgov_up_down_rate_limit(struct hsgov_policy *sg_policy,
					     u64 time, unsigned int next_freq)
{
	s64 delta_ns;

	delta_ns = time - sg_policy->last_freq_update_time;
	if (next_freq > sg_policy->next_freq && delta_ns < sg_policy->up_rate_delay_ns)
		return true;
	if (next_freq < sg_policy->next_freq) {
		s64 down_delay = max(sg_policy->down_rate_delay_ns, sg_policy->down_hold_ns);
		if (delta_ns < down_delay)
			return true;
	}

	return false;
}

static void hsgov_update_commit(struct hsgov_policy *sg_policy,
					u64 time, unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	int cid = arch_get_cluster_id(policy->cpu);

	if (sg_policy->next_freq == next_freq)
		return;

	if (hsgov_up_down_rate_limit(sg_policy, time, next_freq))
		return;

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	if (cpufreq_notifier_fp)
		cpufreq_notifier_fp(cid, next_freq);

#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
	mt_cpufreq_set_by_wfi_load_cluster(cid, next_freq);
	policy->cur = next_freq;
#else
	if (policy->fast_switch_enabled) {
		next_freq = cpufreq_driver_fast_switch(policy, next_freq);
		if (!next_freq)
			return;
		policy->cur = next_freq;
	} else {
		sg_policy->work_in_progress = true;
		irq_work_queue(&sg_policy->irq_work);
	}
#endif
}

static unsigned int hsgov_raw_next_freq(struct hsgov_policy *sg_policy,
					       unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq;

	if (!max)
		return policy->min;

	freq = arch_scale_freq_invariant() ? policy->cpuinfo.max_freq : policy->cur;
	freq = freq * util / max;
	freq = freq / SCHED_CAPACITY_SCALE * capacity_margin;
	sg_policy->cached_raw_freq = freq;

	return hsgov_resolve_freq(policy, freq);
}

static bool hsgov_scene_active(const struct hydrogenx_boost_state *st)
{
	return st->boost || st->input || st->frame || st->gpu || st->ddr || st->io ||
	       st->game_mode || st->profile == HYDROGENX_BOOST_PROFILE_GAME;
}

static unsigned int hsgov_apply_hydrogenx(struct hsgov_policy *sg_policy,
						 unsigned int next_freq,
						 unsigned long util,
						 unsigned long max,
						 unsigned int flags)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	struct hsgov_tunables *tunables = sg_policy->tunables;
	struct hydrogenx_boost_state st;
	unsigned int floor = policy->min;
	unsigned int floor_pct;
	bool active;

	hydrogenx_boost_get_state(&st);
	active = hsgov_scene_active(&st);

	floor_pct = max(READ_ONCE(tunables->floor_pct), st.floor_pct);
	if (READ_ONCE(tunables->hispeed_load) && max &&
	    util * 100U >= max * READ_ONCE(tunables->hispeed_load)) {
		if (READ_ONCE(tunables->hispeed_freq))
			floor = max(floor, READ_ONCE(tunables->hispeed_freq));
		else
			floor_pct = max(floor_pct, HXSGOV_DEFAULT_GAME_FLOOR_PCT);
	}

	if (st.game_mode || st.profile == HYDROGENX_BOOST_PROFILE_GAME) {
		floor_pct = max(floor_pct, HXSGOV_DEFAULT_GAME_FLOOR_PCT);
		if (READ_ONCE(tunables->game_min_freq))
			floor = max(floor, READ_ONCE(tunables->game_min_freq));
	}

	if (st.frame) {
		floor_pct = max(floor_pct, HXSGOV_DEFAULT_FRAME_FLOOR_PCT);
		if (READ_ONCE(tunables->frame_min_freq))
			floor = max(floor, READ_ONCE(tunables->frame_min_freq));
	}

	if (active || (flags & SCHED_CPUFREQ_IOWAIT)) {
		floor_pct = max(floor_pct, READ_ONCE(tunables->boost_floor_pct));
		floor_pct = max(floor_pct, st.floor_pct);
	}

	if ((flags & (SCHED_CPUFREQ_RT | SCHED_CPUFREQ_DL)) &&
	    READ_ONCE(tunables->rt_max_boost))
		return policy->max;

	if ((flags & SCHED_CPUFREQ_IOWAIT) && READ_ONCE(tunables->aggressive_iowait))
		floor_pct = max(floor_pct, READ_ONCE(tunables->boost_floor_pct));

	if (st.floor_freq)
		floor = max(floor, st.floor_freq);
	if (floor_pct)
		floor = max(floor, hsgov_freq_from_pct(policy, floor_pct));

	next_freq = max(next_freq, floor);

	if (st.profile == HYDROGENX_BOOST_PROFILE_BATTERY && !active &&
	    READ_ONCE(tunables->powersave_bias)) {
		unsigned int bias = min(READ_ONCE(tunables->powersave_bias), HXSGOV_MAX_BIAS);
		unsigned int drop = (next_freq - policy->min) * bias / 100U;
		next_freq -= drop;
	}

	return hsgov_resolve_freq(policy, next_freq);
}

static void hsgov_get_util(unsigned long *util, unsigned long *max, int cpu)
{
	unsigned long max_cap;

	max_cap = arch_scale_cpu_capacity(NULL, cpu);
	*util = boosted_cpu_util(cpu);
	if (idle_cpu(cpu))
		*util = 0;
	*util = min(*util, max_cap);
	*max = max_cap;
}

static void hsgov_set_iowait_boost(struct hsgov_cpu *sg_cpu, u64 time,
					   unsigned int flags)
{
	unsigned int max_boost;

	if (flags & SCHED_CPUFREQ_IOWAIT) {
		if (sg_cpu->iowait_boost_pending)
			return;
		sg_cpu->iowait_boost_pending = true;
		max_boost = sg_cpu->iowait_boost_max;
		max_boost = uclamp_util(cpu_rq(sg_cpu->cpu), max_boost);
		if (sg_cpu->iowait_boost) {
			sg_cpu->iowait_boost <<= 1;
			if (sg_cpu->iowait_boost > max_boost)
				sg_cpu->iowait_boost = max_boost;
		} else {
			sg_cpu->iowait_boost = sg_cpu->min_boost;
		}
	} else if (sg_cpu->iowait_boost) {
		s64 delta_ns = time - sg_cpu->last_update;
		if (delta_ns > TICK_NSEC) {
			sg_cpu->iowait_boost = 0;
			sg_cpu->iowait_boost_pending = false;
		}
	}
}

static void hsgov_iowait_boost(struct hsgov_cpu *sg_cpu,
				       unsigned long *util, unsigned long *max)
{
	unsigned int boost_util, boost_max;

	if (!sg_cpu->iowait_boost)
		return;

	if (sg_cpu->iowait_boost_pending) {
		sg_cpu->iowait_boost_pending = false;
	} else {
		sg_cpu->iowait_boost >>= 1;
		if (sg_cpu->iowait_boost < sg_cpu->min_boost) {
			sg_cpu->iowait_boost = 0;
			return;
		}
	}

	boost_util = sg_cpu->iowait_boost;
	boost_max = sg_cpu->iowait_boost_max;
	if (*util * boost_max < *max * boost_util) {
		*util = boost_util;
		*max = boost_max;
	}
}

#ifdef CONFIG_NO_HZ_COMMON
static bool hsgov_cpu_is_busy(struct hsgov_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls_cpu(sg_cpu->cpu);
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool hsgov_cpu_is_busy(struct hsgov_cpu *sg_cpu)
{
	return false;
}
#endif

static void hsgov_update_single(struct update_util_data *hook, u64 time,
					unsigned int flags)
{
	struct hsgov_cpu *sg_cpu = container_of(hook, struct hsgov_cpu, update_util);
	struct hsgov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util, max;
	unsigned int next_f;
	bool busy;
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
	int cid;
#endif

	hsgov_set_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;
	if (!hsgov_should_update_freq(sg_policy, time))
		return;

	busy = hsgov_cpu_is_busy(sg_cpu);
	if (flags & SCHED_CPUFREQ_DL) {
		next_f = policy->cpuinfo.max_freq;
	} else {
		hsgov_get_util(&util, &max, sg_cpu->cpu);
		util = uclamp_util(cpu_rq(sg_cpu->cpu), util);
		hsgov_iowait_boost(sg_cpu, &util, &max);
		next_f = hsgov_raw_next_freq(sg_policy, util, max);
		next_f = hsgov_apply_hydrogenx(sg_policy, next_f, util, max, flags);
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
		next_f = clamp_val(next_f, policy->min, policy->max);
		cid = arch_get_cluster_id(policy->cpu);
		next_f = mt_cpufreq_find_close_freq(cid, next_f);
#endif
		if (busy && next_f < sg_policy->next_freq && sg_policy->next_freq != UINT_MAX) {
			next_f = sg_policy->next_freq;
			sg_policy->cached_raw_freq = 0;
		}
	}

	hsgov_update_commit(sg_policy, time, next_f);
}

static unsigned int hsgov_next_freq_shared(struct hsgov_cpu *sg_cpu, u64 time,
						   unsigned int flags)
{
	struct hsgov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;
	unsigned int next_f;
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
	int cid;
#endif

	for_each_cpu(j, policy->cpus) {
		struct hsgov_cpu *j_sg_cpu = &per_cpu(hsgov_cpu, j);
		unsigned long j_util, j_max;
		s64 delta_ns;

		delta_ns = time - j_sg_cpu->last_update;
		if (delta_ns > TICK_NSEC) {
			j_sg_cpu->iowait_boost = 0;
			j_sg_cpu->iowait_boost_pending = false;
			if (idle_cpu(j))
				continue;
		}

		if (j_sg_cpu->flags & SCHED_CPUFREQ_DL)
			return policy->cpuinfo.max_freq;

		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		j_util = uclamp_util(cpu_rq(j), j_util);
		if (j_util * max > j_max * util) {
			util = j_util;
			max = j_max;
		}
		hsgov_iowait_boost(j_sg_cpu, &util, &max);
	}

	next_f = hsgov_raw_next_freq(sg_policy, util, max);
	next_f = hsgov_apply_hydrogenx(sg_policy, next_f, util, max, flags);
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
	next_f = clamp_val(next_f, policy->min, policy->max);
	cid = arch_get_cluster_id(policy->cpu);
	next_f = mt_cpufreq_find_close_freq(cid, next_f);
#endif
	return next_f;
}

static void hsgov_update_shared(struct update_util_data *hook, u64 time,
					unsigned int flags)
{
	struct hsgov_cpu *sg_cpu = container_of(hook, struct hsgov_cpu, update_util);
	struct hsgov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max;
	unsigned int next_f;

	hsgov_get_util(&util, &max, sg_cpu->cpu);
	raw_spin_lock(&sg_policy->update_lock);
	sg_cpu->util = util;
	sg_cpu->max = max;
	sg_cpu->flags = flags;
	hsgov_set_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	if (hsgov_should_update_freq(sg_policy, time)) {
		if (flags & SCHED_CPUFREQ_DL)
			next_f = sg_policy->policy->cpuinfo.max_freq;
		else
			next_f = hsgov_next_freq_shared(sg_cpu, time, flags);
		hsgov_update_commit(sg_policy, time, next_f);
	}

	raw_spin_unlock(&sg_policy->update_lock);
}

static void hsgov_work(struct kthread_work *work)
{
	struct hsgov_policy *sg_policy = container_of(work, struct hsgov_policy, work);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, sg_policy->next_freq, CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
	sg_policy->work_in_progress = false;
}

static void hsgov_irq_work(struct irq_work *irq_work)
{
	struct hsgov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct hsgov_policy, irq_work);
	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

#define HX_ATTR_SHOW(_name, _field)                                          \
static ssize_t _name##_show(struct gov_attr_set *attr_set, char *buf)        \
{                                                                            \
	struct hsgov_tunables *tunables = to_hsgov_tunables(attr_set);             \
	return sprintf(buf, "%u\n", READ_ONCE(tunables->_field));                \
}

#define HX_ATTR_STORE_SIMPLE(_name, _field, _max)                            \
static ssize_t _name##_store(struct gov_attr_set *attr_set,                  \
				     const char *buf, size_t count)                   \
{                                                                            \
	struct hsgov_tunables *tunables = to_hsgov_tunables(attr_set);             \
	unsigned int val;                                                          \
	if (kstrtouint(buf, 10, &val))                                             \
		return -EINVAL;                                                          \
	if ((_max) && val > (_max))                                                \
		val = (_max);                                                            \
	WRITE_ONCE(tunables->_field, val);                                         \
	return count;                                                              \
}

HX_ATTR_SHOW(hispeed_load, hispeed_load);
HX_ATTR_STORE_SIMPLE(hispeed_load, hispeed_load, 100U);
HX_ATTR_SHOW(hispeed_freq, hispeed_freq);
HX_ATTR_STORE_SIMPLE(hispeed_freq, hispeed_freq, 0U);
HX_ATTR_SHOW(floor_pct, floor_pct);
HX_ATTR_STORE_SIMPLE(floor_pct, floor_pct, 100U);
HX_ATTR_SHOW(game_min_freq, game_min_freq);
HX_ATTR_STORE_SIMPLE(game_min_freq, game_min_freq, 0U);
HX_ATTR_SHOW(frame_min_freq, frame_min_freq);
HX_ATTR_STORE_SIMPLE(frame_min_freq, frame_min_freq, 0U);
HX_ATTR_SHOW(boost_floor_pct, boost_floor_pct);
HX_ATTR_STORE_SIMPLE(boost_floor_pct, boost_floor_pct, 100U);
HX_ATTR_SHOW(powersave_bias, powersave_bias);
HX_ATTR_STORE_SIMPLE(powersave_bias, powersave_bias, 100U);
HX_ATTR_SHOW(aggressive_iowait, aggressive_iowait);
HX_ATTR_STORE_SIMPLE(aggressive_iowait, aggressive_iowait, 1U);
HX_ATTR_SHOW(rt_max_boost, rt_max_boost);
HX_ATTR_STORE_SIMPLE(rt_max_boost, rt_max_boost, 1U);
HX_ATTR_SHOW(debug, debug);
HX_ATTR_STORE_SIMPLE(debug, debug, 1U);

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct hsgov_tunables *tunables = to_hsgov_tunables(attr_set);
	return sprintf(buf, "%u\n", READ_ONCE(tunables->up_rate_limit_us));
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct hsgov_tunables *tunables = to_hsgov_tunables(attr_set);
	return sprintf(buf, "%u\n", READ_ONCE(tunables->down_rate_limit_us));
}

static ssize_t down_hold_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct hsgov_tunables *tunables = to_hsgov_tunables(attr_set);
	return sprintf(buf, "%u\n", READ_ONCE(tunables->down_hold_us));
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct hsgov_tunables *tunables = to_hsgov_tunables(attr_set);
	struct hsgov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	WRITE_ONCE(tunables->up_rate_limit_us, rate_limit_us);
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		hsgov_update_min_rate_limit_ns(sg_policy);
	}
	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct hsgov_tunables *tunables = to_hsgov_tunables(attr_set);
	struct hsgov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	WRITE_ONCE(tunables->down_rate_limit_us, rate_limit_us);
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		hsgov_update_min_rate_limit_ns(sg_policy);
	}
	return count;
}

static ssize_t down_hold_us_store(struct gov_attr_set *attr_set,
				  const char *buf, size_t count)
{
	struct hsgov_tunables *tunables = to_hsgov_tunables(attr_set);
	struct hsgov_policy *sg_policy;
	unsigned int hold_us;

	if (kstrtouint(buf, 10, &hold_us))
		return -EINVAL;

	WRITE_ONCE(tunables->down_hold_us, hold_us);
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook)
		sg_policy->down_hold_ns = hold_us * NSEC_PER_USEC;
	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);
static struct governor_attr down_hold_us = __ATTR_RW(down_hold_us);
static struct governor_attr hispeed_load = __ATTR_RW(hispeed_load);
static struct governor_attr hispeed_freq = __ATTR_RW(hispeed_freq);
static struct governor_attr floor_pct = __ATTR_RW(floor_pct);
static struct governor_attr game_min_freq = __ATTR_RW(game_min_freq);
static struct governor_attr frame_min_freq = __ATTR_RW(frame_min_freq);
static struct governor_attr boost_floor_pct = __ATTR_RW(boost_floor_pct);
static struct governor_attr powersave_bias = __ATTR_RW(powersave_bias);
static struct governor_attr aggressive_iowait = __ATTR_RW(aggressive_iowait);
static struct governor_attr rt_max_boost = __ATTR_RW(rt_max_boost);
static struct governor_attr debug = __ATTR_RW(debug);

static struct attribute *hsgov_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&down_hold_us.attr,
	&hispeed_load.attr,
	&hispeed_freq.attr,
	&floor_pct.attr,
	&game_min_freq.attr,
	&frame_min_freq.attr,
	&boost_floor_pct.attr,
	&powersave_bias.attr,
	&aggressive_iowait.attr,
	&rt_max_boost.attr,
	&debug.attr,
	NULL,
};

static void hsgov_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);
	kfree(to_hsgov_tunables(attr_set));
}

static struct kobj_type hsgov_tunables_ktype = {
	.default_attrs = hsgov_attributes,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &hsgov_tunables_free,
};

static struct hsgov_policy *hsgov_policy_alloc(struct cpufreq_policy *policy)
{
	struct hsgov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;
	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void hsgov_policy_free(struct hsgov_policy *sg_policy)
{
	kfree(sg_policy);
}

static int hsgov_kthread_create(struct hsgov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&sg_policy->work, hsgov_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"hxsugov:%d", cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create hxsugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	if (!policy->dvfs_possible_from_any_cpu)
		kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&sg_policy->irq_work, hsgov_irq_work);
	mutex_init(&sg_policy->work_lock);
	wake_up_process(thread);
	return 0;
}

static void hsgov_kthread_stop(struct hsgov_policy *sg_policy)
{
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

static struct hsgov_tunables *hsgov_tunables_alloc(struct hsgov_policy *sg_policy)
{
	struct hsgov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void hsgov_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
}

static void hsgov_init_defaults(struct hsgov_tunables *tunables,
				       struct cpufreq_policy *policy)
{
	unsigned int delay = cpufreq_policy_transition_delay_us(policy);

	if (!delay)
		delay = HXSGOV_DEFAULT_UP_RATE_US;
	tunables->up_rate_limit_us = min(delay, HXSGOV_DEFAULT_UP_RATE_US);
	tunables->down_rate_limit_us = max(delay, HXSGOV_DEFAULT_DOWN_RATE_US);
	tunables->down_hold_us = HXSGOV_DEFAULT_DOWN_HOLD_US;
	tunables->hispeed_load = HXSGOV_DEFAULT_HISPEED_LOAD;
	tunables->hispeed_freq = 0;
	tunables->floor_pct = 0;
	tunables->game_min_freq = 0;
	tunables->frame_min_freq = 0;
	tunables->boost_floor_pct = HXSGOV_DEFAULT_BOOST_FLOOR_PCT;
	tunables->powersave_bias = 0;
	tunables->aggressive_iowait = 1;
	tunables->rt_max_boost = 1;
	tunables->debug = 0;
}

static int hsgov_init(struct cpufreq_policy *policy)
{
	struct hsgov_policy *sg_policy;
	struct hsgov_tunables *tunables;
	int ret = 0;

	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);
	sg_policy = hsgov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = hsgov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);
	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;
		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = hsgov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}
	hsgov_init_defaults(tunables, policy);
	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;
	ret = kobject_init_and_add(&tunables->attr_set.kobj, &hsgov_tunables_ktype,
					 get_governor_parent_kobj(policy), "%s",
					 hydrogenx_schedutil_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;
fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	hsgov_clear_global_tunables();
stop_kthread:
	hsgov_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);
free_sg_policy:
	hsgov_policy_free(sg_policy);
disable_fast_switch:
	cpufreq_disable_fast_switch(policy);
	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void hsgov_exit(struct cpufreq_policy *policy)
{
	struct hsgov_policy *sg_policy = policy->governor_data;
	struct hsgov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);
	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		hsgov_clear_global_tunables();
	mutex_unlock(&global_tunables_lock);

	hsgov_kthread_stop(sg_policy);
	hsgov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int hsgov_start(struct cpufreq_policy *policy)
{
	struct hsgov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->up_rate_delay_ns = sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns = sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_hold_ns = sg_policy->tunables->down_hold_us * NSEC_PER_USEC;
	hsgov_update_min_rate_limit_ns(sg_policy);
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq = UINT_MAX;
	sg_policy->work_in_progress = false;
	sg_policy->need_freq_update = false;
	sg_policy->cached_raw_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct hsgov_cpu *sg_cpu = &per_cpu(hsgov_cpu, cpu);
		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu = cpu;
		sg_cpu->sg_policy = sg_policy;
		sg_cpu->flags = SCHED_CPUFREQ_DL;
		sg_cpu->iowait_boost_max = capacity_orig_of(cpu);
		sg_cpu->min_boost = (SCHED_CAPACITY_SCALE * policy->cpuinfo.min_freq) /
					     policy->cpuinfo.max_freq;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct hsgov_cpu *sg_cpu = &per_cpu(hsgov_cpu, cpu);
		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
				policy_is_shared(policy) ? hsgov_update_shared : hsgov_update_single);
	}

	return 0;
}

static void hsgov_stop(struct cpufreq_policy *policy)
{
	struct hsgov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);
	synchronize_sched();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}
}

static void hsgov_limits(struct cpufreq_policy *policy)
{
	struct hsgov_policy *sg_policy = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}
	sg_policy->need_freq_update = true;
}

static struct cpufreq_governor hydrogenx_schedutil_gov = {
	.name = "hydrogenx",
	.owner = THIS_MODULE,
	.dynamic_switching = true,
	.init = hsgov_init,
	.exit = hsgov_exit,
	.start = hsgov_start,
	.stop = hsgov_stop,
	.limits = hsgov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_HYDROGENX
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &hydrogenx_schedutil_gov;
}
#endif

static int __init hsgov_register(void)
{
	int ret;

	ret = cpufreq_register_governor(&hydrogenx_schedutil_gov);
	if (!ret)
		pr_info("registered HydrogenX schedutil governor %s\n",
			HYDROGENX_BOOST_VERSION);
	return ret;
}
core_initcall(hsgov_register);
