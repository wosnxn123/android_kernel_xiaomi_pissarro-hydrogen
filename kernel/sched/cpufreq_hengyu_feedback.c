// SPDX-License-Identifier: GPL-2.0
/* Bounded, advisory FPSGO feedback; scheduler utilization remains authoritative. */
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/hengyu.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define HENGYU_FRAME_RECORDS		64
#define HENGYU_FRAME_TARGET_MIN_NS	1000000ULL
#define HENGYU_FRAME_TTL_MAX_NS		100000000ULL
#define HENGYU_FRAME_SAMPLE_MAX_NS	1000000000ULL
#define HENGYU_FRAME_TTL_FRAMES		3U
#define HENGYU_FRAME_TIGHT_MIN		768U /* 75% of CPU target, Q10 */
#define HENGYU_FRAME_CPU_SHARE_MIN	768U /* 75% of Q2Q, Q10 */

struct hengyu_frame_record {
	pid_t pid;
	u64 cookie;
	u64 task_start;
	u64 expires;
	unsigned int pressure;
	cpumask_t cpus;
};

struct hengyu_frame_cache {
	raw_spinlock_t lock;
	u64 expires;
	unsigned int pressure;
};

static DEFINE_RAW_SPINLOCK(hengyu_frame_lock);
static struct hengyu_frame_record hengyu_frames[HENGYU_FRAME_RECORDS];
static DEFINE_PER_CPU(struct hengyu_frame_cache, hengyu_frame_cache) = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(hengyu_frame_cache.lock),
};

/* Caller holds the producer lock. No task or runqueue locks are acquired. */
static void hengyu_frame_rebuild(u64 now)
{
	struct hengyu_frame_cache *cache;
	unsigned int pressure;
	u64 expires;
	int cpu, i;

	for (i = 0; i < HENGYU_FRAME_RECORDS; i++) {
		if (hengyu_frames[i].pid && now >= hengyu_frames[i].expires)
			hengyu_frames[i].pid = 0;
	}

	for_each_possible_cpu(cpu) {
		pressure = 0;
		expires = 0;
		for (i = 0; i < HENGYU_FRAME_RECORDS; i++) {
			struct hengyu_frame_record *frame = &hengyu_frames[i];

			if (!frame->pid || !cpumask_test_cpu(cpu, &frame->cpus))
				continue;
			if (frame->pressure > pressure) {
				pressure = frame->pressure;
				expires = frame->expires;
			} else if (frame->pressure == pressure) {
				expires = max(expires, frame->expires);
			}
		}

		cache = &per_cpu(hengyu_frame_cache, cpu);
		raw_spin_lock(&cache->lock);
		cache->pressure = pressure;
		cache->expires = expires;
		raw_spin_unlock(&cache->lock);
	}
}

unsigned int hengyu_frame_pressure(int cpu, u64 now)
{
	struct hengyu_frame_cache *cache;
	unsigned long flags;
	unsigned int pressure;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return 0;

	cache = &per_cpu(hengyu_frame_cache, cpu);
	raw_spin_lock_irqsave(&cache->lock, flags);
	pressure = now < cache->expires ? cache->pressure : 0;
	raw_spin_unlock_irqrestore(&cache->lock, flags);

	/*
	 * On dominant-hint expiry, return zero until the next producer rebuild,
	 * rather than retaining its pressure or scanning records on a scheduler
	 * callback. Still-live weaker hints may thus be conservatively dropped.
	 */
	return pressure;
}
EXPORT_SYMBOL_GPL(hengyu_frame_pressure);

void hengyu_frame_update(pid_t pid, u64 cookie, const struct cpumask *cpus,
			u64 runtime_ns, u64 target_ns, u64 q2q_ns)
{
	struct hengyu_frame_record *frame = NULL;
	struct task_struct *task;
	unsigned long flags;
	unsigned int ratio, pressure;
	u64 task_start = 0, now, ttl;
	int i, free_slot = -1;
	bool task_found = false;

	/* Validate all operands before multiplying or dividing them. */
	if (pid <= 0 || !cpus || cpumask_empty(cpus) ||
	    target_ns < HENGYU_FRAME_TARGET_MIN_NS ||
	    target_ns > HENGYU_FRAME_TTL_MAX_NS || !runtime_ns ||
	    runtime_ns > HENGYU_FRAME_SAMPLE_MAX_NS || !q2q_ns ||
	    q2q_ns > HENGYU_FRAME_SAMPLE_MAX_NS || runtime_ns > q2q_ns ||
	    runtime_ns * 1024U < q2q_ns * HENGYU_FRAME_CPU_SHARE_MIN) {
		hengyu_frame_remove(pid, cookie);
		return;
	}

	ratio = div64_u64(min(runtime_ns, target_ns + target_ns / 4) *
			 1024U, target_ns);
	if (ratio <= HENGYU_FRAME_TIGHT_MIN) {
		hengyu_frame_remove(pid, cookie);
		return;
	}
	pressure = min((ratio - HENGYU_FRAME_TIGHT_MIN) / 2,
		       HENGYU_FRAME_PRESSURE_MAX);
	if (!pressure) {
		hengyu_frame_remove(pid, cookie);
		return;
	}

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task && !(READ_ONCE(task->flags) & PF_EXITING)) {
		task_start = task->start_time;
		task_found = true;
	}
	rcu_read_unlock();
	if (!task_found) {
		hengyu_frame_remove(pid, cookie);
		return;
	}

	ttl = min(target_ns * HENGYU_FRAME_TTL_FRAMES,
		  HENGYU_FRAME_TTL_MAX_NS);
	raw_spin_lock_irqsave(&hengyu_frame_lock, flags);
	now = ktime_get_ns();
	for (i = 0; i < HENGYU_FRAME_RECORDS; i++) {
		struct hengyu_frame_record *iter = &hengyu_frames[i];

		/* Retire every cookie from an earlier lifetime of this PID. */
		if (iter->pid && (now >= iter->expires ||
		    (iter->pid == pid && iter->task_start != task_start)))
			iter->pid = 0;
		if (iter->pid == pid && iter->cookie == cookie)
			frame = iter;
		if (!iter->pid && free_slot < 0)
			free_slot = i;
	}
	/* Never evict another live renderer when the bounded table is full. */
	if (!frame && free_slot >= 0)
		frame = &hengyu_frames[free_slot];
	if (frame) {
		if (now <= U64_MAX - ttl) {
			frame->pid = pid;
			frame->cookie = cookie;
			frame->task_start = task_start;
			frame->expires = now + ttl;
			frame->pressure = pressure;
			cpumask_and(&frame->cpus, cpus, cpu_possible_mask);
		} else {
			frame->pid = 0;
		}
	}
	hengyu_frame_rebuild(now);
	raw_spin_unlock_irqrestore(&hengyu_frame_lock, flags);
}
EXPORT_SYMBOL_GPL(hengyu_frame_update);

void hengyu_frame_remove(pid_t pid, u64 cookie)
{
	unsigned long flags;
	int i;

	if (pid <= 0)
		return;

	raw_spin_lock_irqsave(&hengyu_frame_lock, flags);
	for (i = 0; i < HENGYU_FRAME_RECORDS; i++) {
		if (hengyu_frames[i].pid == pid &&
		    hengyu_frames[i].cookie == cookie)
			hengyu_frames[i].pid = 0;
	}
	hengyu_frame_rebuild(ktime_get_ns());
	raw_spin_unlock_irqrestore(&hengyu_frame_lock, flags);
}
EXPORT_SYMBOL_GPL(hengyu_frame_remove);

void hengyu_frame_reset(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&hengyu_frame_lock, flags);
	memset(hengyu_frames, 0, sizeof(hengyu_frames));
	hengyu_frame_rebuild(ktime_get_ns());
	raw_spin_unlock_irqrestore(&hengyu_frame_lock, flags);
}
EXPORT_SYMBOL_GPL(hengyu_frame_reset);
