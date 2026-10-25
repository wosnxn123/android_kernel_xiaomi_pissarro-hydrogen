// SPDX-License-Identifier: GPL-2.0
/*
 * Hengyu: scheduler-driven CPU frequency control with bounded prediction.
 *
 * Governor lifecycle and deferred switching follow the schedutil governor
 * by Rafael J. Wysocki, Copyright (C) 2016 Intel Corporation.
 */

#define pr_fmt(fmt) "hengyu: " fmt

#include <linux/cpufreq.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/hengyu.h>
#include <linux/slab.h>
#include <uapi/linux/sched/types.h>
#include <trace/events/power.h>
#include <trace/events/sched.h>

#include "sched.h"
#include "cpufreq_hengyu_platform.h"

#if defined(CONFIG_NONLINEAR_FREQ_CTL) && HENGYU_MTK_FIRMWARE_DVFS
#include "../../drivers/misc/mediatek/base/power/include/mtk_upower.h"
#endif

#define HENGYU_SAMPLE_NS	(2 * NSEC_PER_MSEC)
#define HENGYU_HISTORY_NS	(32 * NSEC_PER_MSEC)
#define HENGYU_MAX_RATE_US	1000000U
#define HENGYU_MAX_HOLD_US	32000U
#define HENGYU_MAX_HEADROOM	50U

unsigned long boosted_cpu_util(int cpu);

static struct cpufreq_governor hengyu_gov;

struct hengyu_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
	unsigned int min_headroom_pct;
	unsigned int max_headroom_pct;
	unsigned int burst_hold_us;
	unsigned int frame_feedback;
};

struct hengyu_policy {
	struct cpufreq_policy *policy;
	struct hengyu_tunables *tunables;
	struct list_head tunables_hook;

	/* Serializes every policy decision, including non-shared policies. */
	raw_spinlock_t update_lock;
	u64 last_freq_update;
	u64 transition_delay_ns;
	u64 up_rate_delay_ns;
	u64 down_rate_delay_ns;
	u64 burst_hold_ns;
	unsigned int min_headroom_pct;
	unsigned int max_headroom_pct;
	unsigned int frame_feedback;
	unsigned int next_freq;
	unsigned int cached_raw_freq;
	unsigned int cached_resolved_freq;
	bool limits_changed;
	bool started;
	bool work_pending;

	/* Unused on the firmware and generic fast-switch paths. */
	struct irq_work irq_work;
	struct kthread_work work;
	struct kthread_worker worker;
	struct task_struct *thread;
	struct mutex work_lock;
};

struct hengyu_cpu {
	struct update_util_data update_util;
	struct hengyu_policy *hp;
	unsigned int cpu;
	unsigned int util;
	unsigned int capacity;
	unsigned int clamp_min;
	unsigned int clamp_max;
	unsigned int flags;
	u64 last_update;

	/* Sampled in time, not once per scheduler callback. */
	u64 sample_time;
	u64 burst_until;
	unsigned int sample_util;
	unsigned int variation;
	unsigned int peak_util;

	unsigned int iowait_boost;
	bool iowait_pending;
};

static DEFINE_PER_CPU(struct hengyu_cpu, hengyu_cpu);
static DEFINE_MUTEX(hengyu_tunables_lock);
static struct hengyu_tunables *hengyu_global_tunables;

static bool hengyu_is_slow(struct hengyu_policy *hp)
{
	return !hengyu_uses_sspm() && !hp->policy->fast_switch_enabled;
}

static void hengyu_notify(struct hengyu_policy *hp, unsigned int freq)
{
	void (*notify)(int, unsigned long) = READ_ONCE(cpufreq_notifier_fp);

	if (notify)
		notify(arch_get_cluster_id(hp->policy->cpu), freq);
}

/* All inputs are CPU-capacity units, except the Q10 frame pressure. */
static unsigned int hengyu_headroom(unsigned int util, unsigned int variation,
				   unsigned int rise, unsigned int pressure,
				   unsigned int min_pct,
				   unsigned int max_pct)
{
	unsigned int floor = util * min_pct / 100;
	unsigned int ceiling = util * max_pct / 100;
	unsigned int margin = floor + max(variation, rise);
	unsigned int frame = util * pressure / SCHED_CAPACITY_SCALE;

	return min(ceiling, max(margin, frame));
}

static void hengyu_sample(struct hengyu_cpu *hc, u64 time, unsigned int util)
{
	struct hengyu_policy *hp = hc->hp;
	s64 elapsed = time - hc->sample_time;
	unsigned int delta, rise, shift, extra;

	if (!hc->sample_time || elapsed < 0 || elapsed > HENGYU_HISTORY_NS) {
		hc->sample_time = time;
		hc->sample_util = util;
		hc->variation = 0;
		extra = util * hp->max_headroom_pct / 100;
		hc->peak_util = min(hc->capacity, util + extra);
		hc->burst_until = time + hp->burst_hold_ns;
		return;
	}

	rise = util > hc->sample_util ? util - hc->sample_util : 0;
	if (rise >= max(1U, hc->capacity / 32)) {
		extra = min(rise, util * hp->max_headroom_pct / 100);
		hc->peak_util = min(hc->capacity, util + extra);
		hc->burst_until = time + hp->burst_hold_ns;
	}

	if (elapsed < HENGYU_SAMPLE_NS)
		return;

	delta = util > hc->sample_util ? util - hc->sample_util :
					hc->sample_util - util;
	shift = min_t(u64, div_u64(elapsed, HENGYU_SAMPLE_NS), 4);
	/* Longer gaps reduce the weight of the old observation. */
	hc->variation = (hc->variation >> shift) + delta - (delta >> shift);
	hc->sample_util = util;
	hc->sample_time = time;
}

static void hengyu_iowait_update(struct hengyu_cpu *hc, u64 time,
				 unsigned int flags, bool active)
{
	s64 elapsed = time - hc->last_update;
	unsigned int floor = max(1U, hc->capacity / 8);
	unsigned int ceiling = max(floor, hc->capacity / 2);

	if (!active || elapsed < 0 || elapsed > TICK_NSEC) {
		hc->iowait_boost = 0;
		hc->iowait_pending = false;
	}
	if (!active || !(flags & SCHED_CPUFREQ_IOWAIT) || hc->iowait_pending)
		return;

	hc->iowait_pending = true;
	hc->iowait_boost = hc->iowait_boost ?
		min(hc->iowait_boost << 1, ceiling) : floor;
}

static unsigned int hengyu_iowait_apply(struct hengyu_cpu *hc, u64 time)
{
	unsigned int boost = hc->iowait_boost;

	if (!boost)
		return 0;
	if ((s64)(time - hc->last_update) > TICK_NSEC) {
		hc->iowait_boost = 0;
		hc->iowait_pending = false;
		return 0;
	}

	if (hc->iowait_pending) {
		hc->iowait_pending = false;
	} else {
		boost >>= 1;
		if (boost < max(1U, hc->capacity / 8))
			boost = 0;
		hc->iowait_boost = boost;
	}
	return boost;
}

static unsigned int hengyu_cpu_demand(struct hengyu_cpu *hc, u64 time,
				     u64 now)
{
	struct hengyu_policy *hp = hc->hp;
	unsigned int util = hc->util;
	unsigned int rise, pressure = 0, margin, demand, ceiling;

	rise = util > hc->sample_util ? util - hc->sample_util : 0;
	if (hp->frame_feedback)
		pressure = hengyu_frame_pressure(hc->cpu, now);
	margin = hengyu_headroom(util, hc->variation, rise, pressure,
				hp->min_headroom_pct, hp->max_headroom_pct);
	demand = min(hc->capacity, util + margin);

	if (time < hc->burst_until) {
		ceiling = util + util * hp->max_headroom_pct / 100;
		demand = max(demand, min(hc->peak_util, ceiling));
	}

	demand = max(demand, hengyu_iowait_apply(hc, time));
	/* Hints and prediction must not inflate a caller's uclamp ceiling. */
	return clamp(demand, hc->clamp_min, hc->clamp_max);
}

static unsigned int hengyu_raw_freq(struct hengyu_policy *hp,
				   unsigned long util, unsigned long capacity)
{
	struct cpufreq_policy *policy = hp->policy;
	unsigned int freq;

#if defined(CONFIG_NONLINEAR_FREQ_CTL) && HENGYU_MTK_FIRMWARE_DVFS
	if (arch_scale_freq_invariant()) {
		struct upower_tbl *tbl = upower_get_core_tbl(policy->cpu);
		int i;

		freq = 0;
		if (tbl) {
			for (i = 0; i < tbl->row_num; i++) {
				if (!tbl->row[i].cap)
					break;
				freq = mt_cpufreq_get_cpu_freq(policy->cpu, i);
				if (tbl->row[i].cap >= util)
					break;
			}
			if (freq)
				return freq;
		}
	}
#endif
	freq = arch_scale_freq_invariant() ? policy->cpuinfo.max_freq :
					    READ_ONCE(policy->cur);
	return div_u64((u64)freq * util, capacity);
}

static unsigned int hengyu_next_freq(struct hengyu_cpu *update, u64 time,
				    bool *idle, bool *deadline)
{
	struct hengyu_policy *hp = update->hp;
	struct cpufreq_policy *policy = hp->policy;
	struct hengyu_cpu *waking = (update->flags & SCHED_CPUFREQ_IOWAIT) ?
				   update : NULL;
	unsigned long util = 0, capacity = 1;
	unsigned int cpu, raw, freq;
	u64 now = hp->frame_feedback ? ktime_get_ns() : 0;

	*idle = true;
	*deadline = false;
	for_each_cpu(cpu, policy->cpus) {
		struct hengyu_cpu *hc = &per_cpu(hengyu_cpu, cpu);
		unsigned int demand;

		if (!READ_ONCE(cpu_rq(cpu)->nr_running) && hc != waking) {
			hc->iowait_boost = 0;
			hc->iowait_pending = false;
			continue;
		}
		*idle = false;
		if (hc->flags & SCHED_CPUFREQ_DL) {
			*deadline = true;
			break;
		}
		demand = hengyu_cpu_demand(hc, time, now);
		if (demand * capacity > util * hc->capacity) {
			util = demand;
			capacity = hc->capacity;
		}
	}

	if (*deadline)
		raw = READ_ONCE(policy->max);
	else if (*idle)
		raw = READ_ONCE(policy->min);
	else
		raw = hengyu_raw_freq(hp, util, capacity);

	if (!hp->limits_changed && raw == hp->cached_raw_freq)
		return hp->cached_resolved_freq;

	freq = hengyu_resolve_freq(policy, raw);
	if (freq) {
		hp->cached_raw_freq = raw;
		hp->cached_resolved_freq = freq;
	}
	return freq;
}

/* Caller holds update_lock; a limit change always bypasses rate limiting. */
static void hengyu_commit(struct hengyu_policy *hp, u64 time,
			  unsigned int next, bool force, bool idle, bool urgent)
{
	struct cpufreq_policy *policy = hp->policy;
	u64 delay;
	unsigned int applied;

	if (!next) {
		hp->limits_changed = true;
		return;
	}
	if (!force && next == hp->next_freq)
		return;

	delay = next > hp->next_freq ? hp->up_rate_delay_ns :
					 hp->down_rate_delay_ns;
	if (!force && !idle && !(urgent && next > hp->next_freq) &&
	    (s64)(time - hp->last_freq_update) < (s64)delay)
		return;

	if (hengyu_uses_sspm()) {
		/* Provider owns policy->cur, architecture scale and power trace. */
		applied = hengyu_sspm_set_freq(policy, next);
		if (!applied)
			goto failed;
		hengyu_notify(hp, applied);
		trace_sched_util(arch_get_cluster_id(policy->cpu), applied, time);
	} else if (policy->fast_switch_enabled) {
		applied = cpufreq_driver_fast_switch(policy, next);
		if (!applied)
			goto failed;
		WRITE_ONCE(policy->cur, applied);
		hengyu_notify(hp, applied);
		trace_cpu_frequency(applied, policy->cpu);
	} else {
		applied = next;
		if (!hp->work_pending) {
			hp->work_pending = true;
			irq_work_queue(&hp->irq_work);
		}
	}

	hp->next_freq = applied;
	hp->last_freq_update = time;
	hp->limits_changed = false;
	return;

failed:
	/* A rejected request is not a committed frequency or a cache hit. */
	hp->limits_changed = true;
	hp->cached_raw_freq = UINT_MAX;
}

static void hengyu_update(struct update_util_data *hook, u64 time,
			  unsigned int flags)
{
	struct hengyu_cpu *hc = container_of(hook, struct hengyu_cpu, update_util);
	struct hengyu_policy *hp = hc->hp;
	struct rq *rq = cpu_rq(hc->cpu);
	unsigned int capacity, util, next;
	bool active, idle, deadline, force, urgent;

	/* The slow-path worker must not manufacture its own RT demand. */
	if (unlikely(current == hp->thread && hc->cpu == smp_processor_id()))
		return;

	capacity = max_t(unsigned long, 1,
			 arch_scale_cpu_capacity(NULL, hc->cpu));
	/* The I/O-wait notification precedes the task's enqueue accounting. */
	active = READ_ONCE(rq->nr_running) != 0 ||
		 (flags & SCHED_CPUFREQ_IOWAIT);
	util = active ? min_t(unsigned long, boosted_cpu_util(hc->cpu),
					   capacity) : 0;

	raw_spin_lock(&hp->update_lock);
	if (unlikely(!hp->started))
		goto unlock;

	if (unlikely(hc->capacity != capacity)) {
		hc->capacity = capacity;
		hc->sample_time = 0;
		hc->iowait_boost = 0;
		hc->iowait_pending = false;
	}
	hc->clamp_min = min(hc->capacity, uclamp_util(rq, 0));
	hc->clamp_max = min(hc->capacity,
			   uclamp_util(rq, SCHED_CAPACITY_SCALE));
	hc->util = util;
	hc->flags = active ? flags : 0;
	hengyu_iowait_update(hc, time, flags, active);
	hc->last_update = time;
	if (active)
		hengyu_sample(hc, time, util);

	if (!hengyu_uses_sspm() && hp->policy->fast_switch_enabled &&
	    !cpufreq_can_do_remote_dvfs(hp->policy))
		goto unlock;

	force = hp->limits_changed || hp->next_freq == UINT_MAX;
	urgent = flags & (SCHED_CPUFREQ_DL | SCHED_CPUFREQ_RT);
	if (!force && active && !urgent &&
	    (s64)(time - hp->last_freq_update) <
		(s64)min(hp->up_rate_delay_ns, hp->down_rate_delay_ns))
		goto unlock;

	next = hengyu_next_freq(hc, time, &idle, &deadline);
	hengyu_commit(hp, time, next, force, idle, urgent || deadline);
unlock:
	raw_spin_unlock(&hp->update_lock);
}

static void hengyu_work(struct kthread_work *work)
{
	struct hengyu_policy *hp = container_of(work, struct hengyu_policy, work);
	unsigned long flags;
	unsigned int freq;
	int ret;

	mutex_lock(&hp->work_lock);
	raw_spin_lock_irqsave(&hp->update_lock, flags);
	freq = hp->next_freq;
	/* A new request may queue this work again while the driver sleeps. */
	hp->work_pending = false;
	raw_spin_unlock_irqrestore(&hp->update_lock, flags);

	ret = __cpufreq_driver_target(hp->policy, freq, CPUFREQ_RELATION_L);
	if (ret) {
		raw_spin_lock_irqsave(&hp->update_lock, flags);
		hp->limits_changed = true;
		hp->cached_raw_freq = UINT_MAX;
		raw_spin_unlock_irqrestore(&hp->update_lock, flags);
	} else {
		hengyu_notify(hp, READ_ONCE(hp->policy->cur));
	}
	mutex_unlock(&hp->work_lock);
}

static void hengyu_irq_work(struct irq_work *irq_work)
{
	struct hengyu_policy *hp = container_of(irq_work, struct hengyu_policy,
					      irq_work);

	kthread_queue_work(&hp->worker, &hp->work);
}

static void hengyu_apply_tunables(struct hengyu_policy *hp)
{
	struct hengyu_tunables *t = hp->tunables;
	unsigned long flags;

	raw_spin_lock_irqsave(&hp->update_lock, flags);
	hp->up_rate_delay_ns = max(hp->transition_delay_ns,
				  (u64)t->up_rate_limit_us * NSEC_PER_USEC);
	hp->down_rate_delay_ns = max(hp->transition_delay_ns,
				    (u64)t->down_rate_limit_us * NSEC_PER_USEC);
	hp->min_headroom_pct = t->min_headroom_pct;
	hp->max_headroom_pct = t->max_headroom_pct;
	hp->burst_hold_ns = (u64)t->burst_hold_us * NSEC_PER_USEC;
	hp->frame_feedback = t->frame_feedback;
	hp->limits_changed = true;
	hp->cached_raw_freq = UINT_MAX;
	raw_spin_unlock_irqrestore(&hp->update_lock, flags);
}

static struct hengyu_tunables *to_hengyu_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct hengyu_tunables, attr_set);
}

enum hengyu_tunable {
	HENGYU_UP_RATE,
	HENGYU_DOWN_RATE,
	HENGYU_MIN_MARGIN,
	HENGYU_MAX_MARGIN,
	HENGYU_BURST_HOLD,
	HENGYU_FRAME_FEEDBACK,
};

static ssize_t hengyu_store(struct gov_attr_set *attr_set, const char *buf,
			    size_t count, enum hengyu_tunable which)
{
	struct hengyu_tunables *t = to_hengyu_tunables(attr_set);
	struct hengyu_policy *hp;
	unsigned int value;

	if (kstrtouint(buf, 10, &value))
		return -EINVAL;

	/* governor_sysfs_ops holds attr_set->update_lock across this store. */
	switch (which) {
	case HENGYU_UP_RATE:
		if (value > HENGYU_MAX_RATE_US)
			return -EINVAL;
		t->up_rate_limit_us = value;
		break;
	case HENGYU_DOWN_RATE:
		if (value > HENGYU_MAX_RATE_US)
			return -EINVAL;
		t->down_rate_limit_us = value;
		break;
	case HENGYU_MIN_MARGIN:
		if (value > t->max_headroom_pct)
			return -EINVAL;
		t->min_headroom_pct = value;
		break;
	case HENGYU_MAX_MARGIN:
		if (value > HENGYU_MAX_HEADROOM || value < t->min_headroom_pct)
			return -EINVAL;
		t->max_headroom_pct = value;
		break;
	case HENGYU_BURST_HOLD:
		if (value > HENGYU_MAX_HOLD_US)
			return -EINVAL;
		t->burst_hold_us = value;
		break;
	case HENGYU_FRAME_FEEDBACK:
		if (value > 1)
			return -EINVAL;
		t->frame_feedback = value;
		break;
	default:
		return -EINVAL;
	}

	list_for_each_entry(hp, &attr_set->policy_list, tunables_hook)
		hengyu_apply_tunables(hp);
	return count;
}

#define HENGYU_ATTR(_name, _which) \
static ssize_t _name##_show(struct gov_attr_set *attr_set, char *buf) \
{ \
	struct hengyu_tunables *t = to_hengyu_tunables(attr_set); \
	return sprintf(buf, "%u\n", READ_ONCE(t->_name)); \
} \
static ssize_t _name##_store(struct gov_attr_set *attr_set, \
			    const char *buf, size_t count) \
{ \
	return hengyu_store(attr_set, buf, count, _which); \
} \
static struct governor_attr _name = __ATTR_RW(_name)

HENGYU_ATTR(up_rate_limit_us, HENGYU_UP_RATE);
HENGYU_ATTR(down_rate_limit_us, HENGYU_DOWN_RATE);
HENGYU_ATTR(min_headroom_pct, HENGYU_MIN_MARGIN);
HENGYU_ATTR(max_headroom_pct, HENGYU_MAX_MARGIN);
HENGYU_ATTR(burst_hold_us, HENGYU_BURST_HOLD);
HENGYU_ATTR(frame_feedback, HENGYU_FRAME_FEEDBACK);

static struct attribute *hengyu_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&min_headroom_pct.attr,
	&max_headroom_pct.attr,
	&burst_hold_us.attr,
	&frame_feedback.attr,
	NULL
};

static void hengyu_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);

	kfree(to_hengyu_tunables(attr_set));
}

static struct kobj_type hengyu_tunables_ktype = {
	.default_attrs = hengyu_attributes,
	.sysfs_ops = &governor_sysfs_ops,
	.release = hengyu_tunables_free,
};

static int hengyu_worker_create(struct hengyu_policy *hp)
{
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct task_struct *thread;
	int ret;

	if (!hengyu_is_slow(hp))
		return 0;

	mutex_init(&hp->work_lock);
	init_irq_work(&hp->irq_work, hengyu_irq_work);
	kthread_init_work(&hp->work, hengyu_work);
	kthread_init_worker(&hp->worker);
	thread = kthread_create(kthread_worker_fn, &hp->worker, "hengyu:%u",
				cpumask_first(hp->policy->related_cpus));
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		goto destroy_mutex;
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		goto destroy_mutex;
	}
	if (!hp->policy->dvfs_possible_from_any_cpu)
		kthread_bind_mask(thread, hp->policy->related_cpus);
	hp->thread = thread;
	wake_up_process(thread);
	return 0;

destroy_mutex:
	mutex_destroy(&hp->work_lock);
	return ret;
}

static void hengyu_worker_destroy(struct hengyu_policy *hp)
{
	if (!hp->thread)
		return;
	kthread_flush_worker(&hp->worker);
	kthread_stop(hp->thread);
	mutex_destroy(&hp->work_lock);
}

static int hengyu_init(struct cpufreq_policy *policy)
{
	struct hengyu_policy *hp;
	struct hengyu_tunables *t;
	int ret;

	if (policy->governor_data)
		return -EBUSY;
	if (!policy->cpuinfo.max_freq)
		return -EINVAL;

	if (!hengyu_uses_sspm())
		cpufreq_enable_fast_switch(policy);
	hp = kzalloc(sizeof(*hp), GFP_KERNEL);
	if (!hp) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}
	hp->policy = policy;
	hp->transition_delay_ns = (u64)cpufreq_policy_transition_delay_us(policy) *
				  NSEC_PER_USEC;
	raw_spin_lock_init(&hp->update_lock);
	ret = hengyu_worker_create(hp);
	if (ret)
		goto free_policy;

	mutex_lock(&hengyu_tunables_lock);
	t = hengyu_global_tunables;
	if (t) {
		hp->tunables = t;
		gov_attr_set_get(&t->attr_set, &hp->tunables_hook);
		policy->governor_data = hp;
		mutex_unlock(&hengyu_tunables_lock);
		return 0;
	}

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		ret = -ENOMEM;
		goto unlock;
	}
	gov_attr_set_init(&t->attr_set, &hp->tunables_hook);
	t->up_rate_limit_us = 500;
	t->down_rate_limit_us = 4000;
	t->min_headroom_pct = 5;
	t->max_headroom_pct = 25;
	t->burst_hold_us = 8000;
	t->frame_feedback = IS_ENABLED(CONFIG_MTK_FPSGO_V3);
	hp->tunables = t;
	policy->governor_data = hp;

	ret = kobject_init_and_add(&t->attr_set.kobj, &hengyu_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   hengyu_gov.name);
	if (ret) {
		policy->governor_data = NULL;
		kobject_put(&t->attr_set.kobj);
		goto unlock;
	}
	if (!have_governor_per_policy())
		hengyu_global_tunables = t;
	mutex_unlock(&hengyu_tunables_lock);
	return 0;

unlock:
	mutex_unlock(&hengyu_tunables_lock);
	hengyu_worker_destroy(hp);
free_policy:
	kfree(hp);
disable_fast_switch:
	if (!hengyu_uses_sspm())
		cpufreq_disable_fast_switch(policy);
	return ret;
}

static void hengyu_exit(struct cpufreq_policy *policy)
{
	struct hengyu_policy *hp = policy->governor_data;
	struct hengyu_tunables *t = hp->tunables;

	mutex_lock(&hengyu_tunables_lock);
	if (!gov_attr_set_put(&t->attr_set, &hp->tunables_hook))
		hengyu_global_tunables = NULL;
	policy->governor_data = NULL;
	mutex_unlock(&hengyu_tunables_lock);

	hengyu_worker_destroy(hp);
	kfree(hp);
	if (!hengyu_uses_sspm())
		cpufreq_disable_fast_switch(policy);
}

static int hengyu_start(struct cpufreq_policy *policy)
{
	struct hengyu_policy *hp = policy->governor_data;
	unsigned long flags;
	unsigned int cpu;

	mutex_lock(&hp->tunables->attr_set.update_lock);
	hengyu_apply_tunables(hp);
	mutex_unlock(&hp->tunables->attr_set.update_lock);

	for_each_cpu(cpu, policy->cpus) {
		struct hengyu_cpu *hc = &per_cpu(hengyu_cpu, cpu);

		memset(hc, 0, sizeof(*hc));
		hc->hp = hp;
		hc->cpu = cpu;
		hc->capacity = max_t(unsigned long, 1,
				    arch_scale_cpu_capacity(NULL, cpu));
		hc->clamp_max = hc->capacity;
	}

	raw_spin_lock_irqsave(&hp->update_lock, flags);
	hp->last_freq_update = 0;
	hp->next_freq = UINT_MAX;
	hp->cached_raw_freq = UINT_MAX;
	hp->cached_resolved_freq = 0;
	hp->limits_changed = true;
	hp->work_pending = false;
	hp->started = true;
	raw_spin_unlock_irqrestore(&hp->update_lock, flags);

	for_each_cpu(cpu, policy->cpus) {
		struct hengyu_cpu *hc = &per_cpu(hengyu_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &hc->update_util, hengyu_update);
	}
	return 0;
}

static void hengyu_stop(struct cpufreq_policy *policy)
{
	struct hengyu_policy *hp = policy->governor_data;
	unsigned long flags;
	unsigned int cpu;

	raw_spin_lock_irqsave(&hp->update_lock, flags);
	hp->started = false;
	raw_spin_unlock_irqrestore(&hp->update_lock, flags);
	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);
	synchronize_sched();

	if (hengyu_is_slow(hp)) {
		irq_work_sync(&hp->irq_work);
		kthread_cancel_work_sync(&hp->work);
	}
}

static void hengyu_limits(struct cpufreq_policy *policy)
{
	struct hengyu_policy *hp = policy->governor_data;
	unsigned long flags;
	unsigned int freq;

	if (hengyu_is_slow(hp)) {
		mutex_lock(&hp->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&hp->work_lock);
	}

	raw_spin_lock_irqsave(&hp->update_lock, flags);
	hp->limits_changed = true;
	hp->cached_raw_freq = UINT_MAX;
	if (!hengyu_is_slow(hp) && hp->started &&
	    (hengyu_uses_sspm() || cpufreq_can_do_remote_dvfs(policy))) {
		freq = hengyu_resolve_freq(policy, READ_ONCE(policy->cur));
		hengyu_commit(hp, hp->last_freq_update, freq, true, false, false);
		/* Re-evaluate demand on the next callback, even at the same OPP. */
		hp->limits_changed = true;
	}
	raw_spin_unlock_irqrestore(&hp->update_lock, flags);
}

static struct cpufreq_governor hengyu_gov = {
	.name = "hengyu",
	.owner = THIS_MODULE,
	.dynamic_switching = true,
	.init = hengyu_init,
	.exit = hengyu_exit,
	.start = hengyu_start,
	.stop = hengyu_stop,
	.limits = hengyu_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_HENGYU
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &hengyu_gov;
}
#endif

static int __init hengyu_register(void)
{
	return cpufreq_register_governor(&hengyu_gov);
}
core_initcall(hengyu_register);
