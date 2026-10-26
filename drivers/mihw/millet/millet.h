/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mi frozen framework (millet) - shared definitions
 *
 * Copyright (c) Xiaomi Technologies Co., Ltd. 2019-2020.
 */
#ifndef _MILLET_H
#define _MILLET_H

#include <linux/atomic.h>
#include <linux/cgroup.h>
#include <linux/freezer.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <net/sock.h>

#include "uapi/millet.h"

#define MILLET_WARN_AHEAD_MSGS		3
#define MILLET_WARN_AHEAD_SPACE		(1 << 17)
#define MILLET_TX_MAX			1024
#define MILLET_PKG_MAX_UIDS		64

extern unsigned int millet_debug;
extern int frozen_uid_min;
extern unsigned long binder_warn_ahead_space;

/*
 * Queue a report for the userspace monitor.
 *
 * This is safe in atomic context (tracepoints, netfilter hooks, signal
 * delivery): it only allocates with GFP_ATOMIC and takes a raw spinlock.
 * The netlink send itself is done by a workqueue.
 *
 * @data must be fully resolved by the caller; kernel pointers are never
 * passed to userspace.
 */
int millet_report(enum MILLET_TYPE type, const struct millet_data *data);

/* MSG_TO_KERN handlers, run in the sender's process context. */
void millet_binder_st_recv(const struct millet_userconf *msg, unsigned int len);
void millet_pkg_recv(const struct millet_userconf *msg, unsigned int len);

static inline bool millet_task_frozen(struct task_struct *task)
{
	return task && (freezing(task) || frozen(task) ||
			cgroup_task_freeze(task) || cgroup_task_frozen(task));
}

#endif /* _MILLET_H */
