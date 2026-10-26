// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mi frozen framework (millet) - signal events
 *
 * Copyright (c) Xiaomi Technologies Co., Ltd. 2019-2020.
 */

#define pr_fmt(fmt) "millet-sig: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <trace/hooks/signal.h>

#include "millet.h"

static atomic_t millet_last_killed_pid = ATOMIC_INIT(0);

static void millet_sig_hook(void *data, int sig, struct task_struct *killer,
			    struct task_struct *dst)
{
	struct millet_data msg;
	int pid;

	if (!dst)
		return;

	switch (sig) {
	case SIGKILL:
	case SIGTERM:
	case SIGABRT:
	case SIGQUIT:
		break;
	default:
		return;
	}

	if (!millet_task_frozen(dst))
		return;

	pid = task_tgid_nr(dst);
	if (atomic_xchg(&millet_last_killed_pid, pid) == pid)
		return;

	memset(&msg, 0, sizeof(msg));
	msg.msg_type = MSG_TO_USER;
	msg.uid = task_uid(dst).val;
	msg.pri[0] = KILLED_BY_PRO;
	msg.mod.k_priv.sig.killed_pid = pid;
	msg.mod.k_priv.sig.reason = KILLED_BY_PRO;
	millet_report(SIG_TYPE, &msg);
}

static int __init millet_sig_init(void)
{
	register_trace_android_vh_do_send_sig_info(millet_sig_hook, NULL);
	pr_info("initialized\n");
	return 0;
}
module_init(millet_sig_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mi frozen framework signal events");
