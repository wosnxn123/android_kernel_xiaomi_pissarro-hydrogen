/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_HENGYU_H
#define _LINUX_SCHED_HENGYU_H

#include <linux/cpumask.h>
#include <linux/types.h>

/* Q10 proportional headroom, never an absolute utilization floor. */
#define HENGYU_FRAME_PRESSURE_MAX 256U

#ifdef CONFIG_CPU_FREQ_GOV_HENGYU
unsigned int hengyu_frame_pressure(int cpu, u64 now);
void hengyu_frame_update(pid_t pid, u64 cookie, const struct cpumask *cpus,
			u64 runtime_ns, u64 target_ns, u64 q2q_ns);
void hengyu_frame_remove(pid_t pid, u64 cookie);
void hengyu_frame_reset(void);
#else
static inline unsigned int hengyu_frame_pressure(int cpu, u64 now)
{
	return 0;
}

static inline void hengyu_frame_update(pid_t pid, u64 cookie,
		const struct cpumask *cpus, u64 runtime_ns, u64 target_ns,
		u64 q2q_ns)
{
}

static inline void hengyu_frame_remove(pid_t pid, u64 cookie)
{
}

static inline void hengyu_frame_reset(void)
{
}
#endif

#endif /* _LINUX_SCHED_HENGYU_H */
