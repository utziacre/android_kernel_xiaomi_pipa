// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mi frozen framework (millet) - freezer cgroup permission check
 *
 * Only the system UID or a CAP_SYS_ADMIN caller may move another task
 * into the freezer cgroup.
 *
 * Copyright (c) Xiaomi Technologies Co., Ltd. 2019-2020.
 */

#define pr_fmt(fmt) "millet-cgroup: " fmt

#include <linux/capability.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>

#include "millet.h"

extern struct cgroup_subsys freezer_cgrp_subsys;

static int (*millet_orig_can_attach)(struct cgroup_taskset *tset);
static void (*millet_orig_cancel_attach)(struct cgroup_taskset *tset);

static int millet_can_attach(struct cgroup_taskset *tset)
{
	const struct cred *cred = current_cred();
	bool privileged = cred->euid.val == 1000 || capable(CAP_SYS_ADMIN);
	struct cgroup_subsys_state *css;
	struct task_struct *task;

	cgroup_taskset_for_each(task, css, tset) {
		if (current == task || privileged)
			continue;
		pr_err("%s: permission denied\n", task->comm);
		return -EPERM;
	}

	return 0;
}

static int millet_freezer_can_attach(struct cgroup_taskset *tset)
{
	int ret;

	ret = millet_can_attach(tset);
	if (ret)
		return ret;
	if (millet_orig_can_attach)
		return millet_orig_can_attach(tset);
	return 0;
}

static void millet_freezer_cancel_attach(struct cgroup_taskset *tset)
{
	if (millet_orig_cancel_attach)
		millet_orig_cancel_attach(tset);
}

static int __init millet_cgroup_init(void)
{
	millet_orig_can_attach = freezer_cgrp_subsys.can_attach;
	millet_orig_cancel_attach = freezer_cgrp_subsys.cancel_attach;
	freezer_cgrp_subsys.can_attach = millet_freezer_can_attach;
	freezer_cgrp_subsys.cancel_attach = millet_freezer_cancel_attach;

	pr_info("initialized\n");
	return 0;
}
module_init(millet_cgroup_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mi frozen framework freezer permission check");
