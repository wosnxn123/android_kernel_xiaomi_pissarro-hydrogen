// SPDX-License-Identifier: GPL-2.0
/*
 * HydrogenX patch canary for pissarro.
 *
 * This file intentionally builds unconditionally through kernel/sched/Makefile.
 * It does not change scheduling policy; it only proves that the HydrogenX
 * patch was actually applied and compiled into the image. If your CI log
 * does not show "CC kernel/sched/hydrogenx_canary.o" and dmesg does not contain
 * the messages below, the patch did not enter the kernel build.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/export.h>

#define HX_CANARY_VERSION "v13-hydrogen-schedutil-fixup"

const char hydrogenx_patch_canary[] = "HydrogenX patch applied: " HX_CANARY_VERSION;
EXPORT_SYMBOL_GPL(hydrogenx_patch_canary);

static int __init hydrogenx_canary_early(void)
{
	pr_info("HydrogenX canary early: source patch is present (%s)\n",
		HX_CANARY_VERSION);
	return 0;
}
pure_initcall(hydrogenx_canary_early);

static int __init hydrogenx_canary_init(void)
{
	pr_info("HydrogenX canary: patch applied and built (%s)\n",
		HX_CANARY_VERSION);
	pr_info("HydrogenX canary: expect governor=hydrogenx and /sys/kernel/hydrogenx when configs are enabled\n");
	return 0;
}
late_initcall(hydrogenx_canary_init);

