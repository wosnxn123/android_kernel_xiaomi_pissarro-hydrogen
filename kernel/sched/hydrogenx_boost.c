
// SPDX-License-Identifier: GPL-2.0
/*
 * HydrogenX boost scene controller.
 *
 * v12 is scheduler-hotpath safe: hydrogenx_boost_get_state() is lockless and
 * may be called from schedutil update callbacks while rq locks are held.
 * Sysfs writes and input events update state with WRITE_ONCE() and schedule a
 * worker for heavier SchedTune/UCLAMP/vendor bridge calls.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/compiler.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/hydrogenx_boost.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define HX_DEF_BOOST_MS          180U
#define HX_DEF_INPUT_MS          96U
#define HX_DEF_FRAME_MS          280U
#define HX_DEF_GPU_MS            220U
#define HX_DEF_DDR_MS            220U
#define HX_DEF_IO_MS             180U
#define HX_DEF_GAME_FLOOR_PCT    62U
#define HX_DEF_BOOST_FLOOR_PCT   72U
#define HX_DEF_SCHED_BOOST       12U
#define HX_DEF_UCLAMP_MIN        20U
#define HX_MAX_MS                10000U
#define HX_MAX_BOOST             100U

struct hx_ctrl {
	struct mutex lock;
	struct kobject *kobj;
	struct work_struct scene_work;
	unsigned long until[HYDROGENX_BOOST_MAX];
	unsigned int duration_ms[HYDROGENX_BOOST_MAX];
	unsigned int profile;
	unsigned int game_mode;
	unsigned int floor_freq;
	unsigned int floor_pct;
	unsigned int boost_floor_pct;
	unsigned int sched_boost;
	unsigned int uclamp_min;
	unsigned int input_hook;
};

static struct hx_ctrl hx;

void __weak schedtune_hydrogenx_set_boost(int boost) { }
void __weak schedtune_hydrogenx_set_uclamp_min(unsigned int pct) { }
void __weak mtk_hydrogenx_scene_boost(unsigned int scene, unsigned int active) { }

static bool hx_active(unsigned int reason)
{
	if (reason >= HYDROGENX_BOOST_MAX)
		return false;
	return time_before(jiffies, READ_ONCE(hx.until[reason]));
}

static void hx_fill_state(struct hydrogenx_boost_state *state)
{
	memset(state, 0, sizeof(*state));
	state->boost = hx_active(HYDROGENX_BOOST_GENERIC);
	state->input = hx_active(HYDROGENX_BOOST_INPUT);
	state->frame = hx_active(HYDROGENX_BOOST_FRAME);
	state->gpu = hx_active(HYDROGENX_BOOST_GPU);
	state->ddr = hx_active(HYDROGENX_BOOST_DDR);
	state->io = hx_active(HYDROGENX_BOOST_IO);
	state->profile = READ_ONCE(hx.profile);
	state->game_mode = READ_ONCE(hx.game_mode);
	state->sched_boost = READ_ONCE(hx.sched_boost);
	state->uclamp_min = READ_ONCE(hx.uclamp_min);
	state->floor_freq = READ_ONCE(hx.floor_freq);
	state->floor_pct = READ_ONCE(hx.floor_pct);

	if (state->profile == HYDROGENX_BOOST_PROFILE_GAME || state->game_mode)
		state->floor_pct = max_t(unsigned int, state->floor_pct, HX_DEF_GAME_FLOOR_PCT);
	if (state->boost || state->input || state->frame || state->gpu || state->ddr || state->io)
		state->floor_pct = max_t(unsigned int, state->floor_pct,
					  READ_ONCE(hx.boost_floor_pct));
	if (state->profile == HYDROGENX_BOOST_PROFILE_BATTERY && !state->boost &&
	    !state->input && !state->frame)
		state->floor_pct = 0;
}

void hydrogenx_boost_get_state(struct hydrogenx_boost_state *state)
{
	hx_fill_state(state);
}
EXPORT_SYMBOL_GPL(hydrogenx_boost_get_state);

static void hx_apply_scene_work(struct work_struct *work)
{
	struct hydrogenx_boost_state st;
	bool active;

	hx_fill_state(&st);
	active = st.boost || st.input || st.frame || st.gpu || st.ddr || st.io ||
		 st.game_mode || st.profile == HYDROGENX_BOOST_PROFILE_GAME;

	schedtune_hydrogenx_set_boost(active ? st.sched_boost : 0);
	schedtune_hydrogenx_set_uclamp_min(active ? st.uclamp_min : 0);
	mtk_hydrogenx_scene_boost(HYDROGENX_BOOST_GENERIC, st.boost);
	mtk_hydrogenx_scene_boost(HYDROGENX_BOOST_INPUT, st.input);
	mtk_hydrogenx_scene_boost(HYDROGENX_BOOST_FRAME, st.frame);
	mtk_hydrogenx_scene_boost(HYDROGENX_BOOST_GPU, st.gpu);
	mtk_hydrogenx_scene_boost(HYDROGENX_BOOST_DDR, st.ddr);
	mtk_hydrogenx_scene_boost(HYDROGENX_BOOST_IO, st.io);
}

void hydrogenx_boost_pulse(enum hydrogenx_boost_reason reason, unsigned int duration_ms)
{
	if (reason >= HYDROGENX_BOOST_MAX)
		return;
	if (!duration_ms)
		duration_ms = READ_ONCE(hx.duration_ms[reason]);
	if (duration_ms > HX_MAX_MS)
		duration_ms = HX_MAX_MS;
	WRITE_ONCE(hx.until[reason], jiffies + msecs_to_jiffies(duration_ms));
	schedule_work(&hx.scene_work);
}
EXPORT_SYMBOL_GPL(hydrogenx_boost_pulse);

static int hx_parse_uint(const char *buf, unsigned int *val)
{
	return kstrtouint(buf, 0, val);
}

#define HX_RW_ATTR(_name, _field, _max)                                      \
static ssize_t _name##_show(struct kobject *kobj, struct kobj_attribute *a,  \
				    char *buf)                                      \
{                                                                            \
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(hx._field));           \
}                                                                            \
static ssize_t _name##_store(struct kobject *kobj, struct kobj_attribute *a, \
				     const char *buf, size_t count)                    \
{                                                                            \
	unsigned int val;                                                          \
	if (hx_parse_uint(buf, &val))                                              \
		return -EINVAL;                                                          \
	if ((_max) && val > (_max))                                                \
		val = (_max);                                                            \
	mutex_lock(&hx.lock);                                                      \
	WRITE_ONCE(hx._field, val);                                                \
	mutex_unlock(&hx.lock);                                                    \
	schedule_work(&hx.scene_work);                                             \
	return count;                                                              \
}                                                                            \
static struct kobj_attribute _name##_attr = __ATTR_RW(_name)

HX_RW_ATTR(profile, profile, HYDROGENX_BOOST_PROFILE_BATTERY);
HX_RW_ATTR(game_mode, game_mode, 1U);
HX_RW_ATTR(floor_freq, floor_freq, 0U);
HX_RW_ATTR(floor_pct, floor_pct, 100U);
HX_RW_ATTR(boost_floor_pct, boost_floor_pct, 100U);
HX_RW_ATTR(sched_boost, sched_boost, HX_MAX_BOOST);
HX_RW_ATTR(uclamp_min, uclamp_min, HX_MAX_BOOST);
HX_RW_ATTR(input_hook, input_hook, 1U);

#define HX_PULSE_ATTR(_name, _reason)                                        \
static ssize_t _name##_show(struct kobject *kobj, struct kobj_attribute *a,  \
				    char *buf)                                      \
{                                                                            \
	return scnprintf(buf, PAGE_SIZE, "%u\n", hx_active(_reason));             \
}                                                                            \
static ssize_t _name##_store(struct kobject *kobj, struct kobj_attribute *a, \
				     const char *buf, size_t count)                    \
{                                                                            \
	unsigned int val = 1;                                                       \
	hx_parse_uint(buf, &val);                                                   \
	if (val)                                                                   \
		hydrogenx_boost_pulse(_reason, 0);                                       \
	return count;                                                              \
}                                                                            \
static struct kobj_attribute _name##_attr = __ATTR_RW(_name)

HX_PULSE_ATTR(boostpulse, HYDROGENX_BOOST_GENERIC);
HX_PULSE_ATTR(input_boostpulse, HYDROGENX_BOOST_INPUT);
HX_PULSE_ATTR(frame_boost, HYDROGENX_BOOST_FRAME);
HX_PULSE_ATTR(gpu_boost, HYDROGENX_BOOST_GPU);
HX_PULSE_ATTR(ddr_boost, HYDROGENX_BOOST_DDR);
HX_PULSE_ATTR(io_boost, HYDROGENX_BOOST_IO);

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *a, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", HYDROGENX_BOOST_VERSION);
}
static struct kobj_attribute version_attr = __ATTR_RO(version);

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *a, char *buf)
{
	struct hydrogenx_boost_state st;

	hx_fill_state(&st);
	return scnprintf(buf, PAGE_SIZE,
		"version=%s profile=%u game_mode=%u floor_freq=%u floor_pct=%u "
		"sched_boost=%u uclamp_min=%u boost=%u input=%u frame=%u gpu=%u ddr=%u io=%u\n",
		HYDROGENX_BOOST_VERSION, st.profile, st.game_mode, st.floor_freq,
		st.floor_pct, st.sched_boost, st.uclamp_min, st.boost, st.input,
		st.frame, st.gpu, st.ddr, st.io);
}
static struct kobj_attribute status_attr = __ATTR_RO(status);

static ssize_t builtins_show(struct kobject *kobj, struct kobj_attribute *a, char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
		"schedutil_core=1 hotpath_lockless=1 input_hook=1 schedtune_bridge=1 "
		"uclamp_bridge=1 weak_mtk_bridge=1 zram_lz4=1 f2fs_compression=1 erofs_zip_lz4=1\n");
}
static struct kobj_attribute builtins_attr = __ATTR_RO(builtins);

static struct attribute *hx_attrs[] = {
	&profile_attr.attr,
	&game_mode_attr.attr,
	&floor_freq_attr.attr,
	&floor_pct_attr.attr,
	&boost_floor_pct_attr.attr,
	&sched_boost_attr.attr,
	&uclamp_min_attr.attr,
	&input_hook_attr.attr,
	&boostpulse_attr.attr,
	&input_boostpulse_attr.attr,
	&frame_boost_attr.attr,
	&gpu_boost_attr.attr,
	&ddr_boost_attr.attr,
	&io_boost_attr.attr,
	&version_attr.attr,
	&status_attr.attr,
	&builtins_attr.attr,
	NULL,
};

static const struct attribute_group hx_attr_group = {
	.attrs = hx_attrs,
};

static void hx_input_event(struct input_handle *handle, unsigned int type,
			   unsigned int code, int value)
{
	if (!READ_ONCE(hx.input_hook) || !value)
		return;
	if (type == EV_ABS || type == EV_REL || type == EV_KEY)
		hydrogenx_boost_pulse(HYDROGENX_BOOST_INPUT, 0);
}

static int hx_input_connect(struct input_handler *handler, struct input_dev *dev,
			    const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "hydrogenx";

	error = input_register_handle(handle);
	if (error)
		goto err_free;
	error = input_open_device(handle);
	if (error)
		goto err_unregister;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return error;
}

static void hx_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id hx_input_ids[] = {
	{ .driver_info = 1 },
	{ },
};
MODULE_DEVICE_TABLE(input, hx_input_ids);

static struct input_handler hx_input_handler = {
	.event = hx_input_event,
	.connect = hx_input_connect,
	.disconnect = hx_input_disconnect,
	.name = "hydrogenx_input_boost",
	.id_table = hx_input_ids,
};

static int __init hydrogenx_boost_init(void)
{
	int ret;

	mutex_init(&hx.lock);
	INIT_WORK(&hx.scene_work, hx_apply_scene_work);
	hx.duration_ms[HYDROGENX_BOOST_GENERIC] = HX_DEF_BOOST_MS;
	hx.duration_ms[HYDROGENX_BOOST_INPUT] = HX_DEF_INPUT_MS;
	hx.duration_ms[HYDROGENX_BOOST_FRAME] = HX_DEF_FRAME_MS;
	hx.duration_ms[HYDROGENX_BOOST_GPU] = HX_DEF_GPU_MS;
	hx.duration_ms[HYDROGENX_BOOST_DDR] = HX_DEF_DDR_MS;
	hx.duration_ms[HYDROGENX_BOOST_IO] = HX_DEF_IO_MS;
	hx.profile = HYDROGENX_BOOST_PROFILE_BALANCED;
	hx.game_mode = 0;
	hx.floor_freq = 0;
	hx.floor_pct = 0;
	hx.boost_floor_pct = HX_DEF_BOOST_FLOOR_PCT;
	hx.sched_boost = HX_DEF_SCHED_BOOST;
	hx.uclamp_min = HX_DEF_UCLAMP_MIN;
	hx.input_hook = 1;

	hx.kobj = kobject_create_and_add("hydrogenx", kernel_kobj);
	if (!hx.kobj)
		return -ENOMEM;

	ret = sysfs_create_group(hx.kobj, &hx_attr_group);
	if (ret)
		goto err_put;

	ret = input_register_handler(&hx_input_handler);
	if (ret)
		pr_warn("input hook registration failed: %d\n", ret);

	pr_info("HydrogenX boost controller %s ready\n", HYDROGENX_BOOST_VERSION);
	return 0;

err_put:
	kobject_put(hx.kobj);
	return ret;
}
late_initcall(hydrogenx_boost_init);
