
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HYDROGENX_BOOST_H
#define _LINUX_HYDROGENX_BOOST_H

#include <linux/types.h>
#include <linux/string.h>

#define HYDROGENX_BOOST_PROFILE_BALANCED 0
#define HYDROGENX_BOOST_PROFILE_GAME     1
#define HYDROGENX_BOOST_PROFILE_BATTERY  2
#define HYDROGENX_BOOST_VERSION          "v13-hydrogen-schedutil-fixup"
#define HYDROGENX_BOOST_VERSION_CODE     13

enum hydrogenx_boost_reason {
	HYDROGENX_BOOST_GENERIC = 0,
	HYDROGENX_BOOST_INPUT,
	HYDROGENX_BOOST_FRAME,
	HYDROGENX_BOOST_GPU,
	HYDROGENX_BOOST_DDR,
	HYDROGENX_BOOST_IO,
	HYDROGENX_BOOST_MAX,
};

struct hydrogenx_boost_state {
	bool boost;
	bool input;
	bool frame;
	bool gpu;
	bool ddr;
	bool io;
	unsigned int profile;
	unsigned int game_mode;
	unsigned int sched_boost;
	unsigned int uclamp_min;
	unsigned int floor_freq;
	unsigned int floor_pct;
};

#ifdef CONFIG_CPU_FREQ_GOV_HYDROGENX
void hydrogenx_boost_pulse(enum hydrogenx_boost_reason reason, unsigned int duration_ms);
void hydrogenx_boost_get_state(struct hydrogenx_boost_state *state);
#else
static inline void hydrogenx_boost_pulse(enum hydrogenx_boost_reason reason,
					 unsigned int duration_ms) { }
static inline void hydrogenx_boost_get_state(struct hydrogenx_boost_state *state)
{
	memset(state, 0, sizeof(*state));
	state->profile = HYDROGENX_BOOST_PROFILE_BALANCED;
}
#endif

#endif /* _LINUX_HYDROGENX_BOOST_H */
