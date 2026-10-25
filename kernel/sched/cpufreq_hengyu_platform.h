/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCHED_CPUFREQ_HENGYU_PLATFORM_H
#define _SCHED_CPUFREQ_HENGYU_PLATFORM_H

#include <linux/cpufreq.h>

/*
 * MT6877's v1 provider defines CONFIG_HYBRID_CPU_DVFS locally from MCUPM,
 * not from SSPM support. SSPM alone leaves the public request API a no-op.
 * Other platforms use the normal cpufreq driver paths until their provider
 * guards and request semantics have been established independently.
 */
#if defined(CONFIG_MTK_CPU_FREQ) && defined(CONFIG_MACH_MT6877) && \
	defined(CONFIG_MTK_TINYSYS_MCUPM_SUPPORT)
#define HENGYU_MTK_FIRMWARE_DVFS 1
#include <linux/topology.h>
#include "../../drivers/misc/mediatek/include/mt-plat/mtk_cpufreq_common_api.h"
#else
#define HENGYU_MTK_FIRMWARE_DVFS 0
#endif

static inline bool hengyu_uses_sspm(void)
{
	return HENGYU_MTK_FIRMWARE_DVFS;
}

/*
 * Select the lowest legal OPP at or above the target. If rounding upward
 * would cross policy->max, use the highest legal OPP below the target.
 * An empty legal interval returns zero, never a non-OPP clamped frequency.
 * The caller serializes policy requests and governor limits processing.
 */
static inline unsigned int
hengyu_resolve_freq(struct cpufreq_policy *policy, unsigned int target_freq)
{
	struct cpufreq_frequency_table *pos;
	unsigned int min_freq = READ_ONCE(policy->min);
	unsigned int max_freq = READ_ONCE(policy->max);
	unsigned int above = 0, below = 0, freq;

	if (!max_freq || min_freq > max_freq)
		return 0;

	target_freq = clamp_val(target_freq, min_freq, max_freq);
	if (!policy->freq_table) {
		freq = cpufreq_driver_resolve_freq(policy, target_freq);
		return freq >= min_freq && freq <= max_freq ? freq : 0;
	}

	cpufreq_for_each_valid_entry(pos, policy->freq_table) {
		freq = pos->frequency;
		if (!freq || freq < min_freq || freq > max_freq)
			continue;

		if (freq >= target_freq) {
			if (!above || freq < above)
				above = freq;
		} else if (freq > below) {
			below = freq;
		}
	}

	return above ? above : below;
}

/*
 * Return an accepted nonzero request in kHz, or zero on rejection. The
 * provider writes the WFI OPP into shared SRAM without sleeping; firmware
 * applies it asynchronously under its own limits. Its zero status is not
 * a hardware acknowledgement and no measured clock is returned.
 *
 * Keep the established MT6877 provider bookkeeping: the public wrapper
 * invokes its CM-manager sampler, updates policy->cur and arch frequency
 * scale, and emits cpu_frequency traces for policy CPUs. The governor must
 * not duplicate those actions here. It still owns its next_freq/time,
 * the shared FPSGO notifier and its scheduler-specific trace.
 */
static inline unsigned int
hengyu_sspm_set_freq(struct cpufreq_policy *policy, unsigned int target_freq)
{
#if HENGYU_MTK_FIRMWARE_DVFS
	unsigned int freq, min_freq, max_freq;
	int cluster = arch_get_cluster_id(policy->cpu);

	/* MT6877 has two CPU clusters; the next provider ID belongs to CCI. */
	if (cluster < 0 || cluster >= 2 || !policy->freq_table)
		return 0;

	freq = hengyu_resolve_freq(policy, target_freq);
	if (!freq)
		return 0;

	/* These are the same endpoint indices clamped by the public wrapper. */
	min_freq = mt_cpufreq_get_freq_by_idx(cluster, 15);
	max_freq = mt_cpufreq_get_freq_by_idx(cluster, 0);
	if (!min_freq || !max_freq || freq < min_freq || freq > max_freq)
		return 0;

	if (mt_cpufreq_find_close_freq(cluster, freq) != freq)
		return 0;

	if (mt_cpufreq_set_by_wfi_load_cluster(cluster, freq))
		return 0;

	return freq;
#else
	return 0;
#endif
}

#endif /* _SCHED_CPUFREQ_HENGYU_PLATFORM_H */
