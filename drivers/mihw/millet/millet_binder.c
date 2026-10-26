// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mi frozen framework (millet) - binder events
 *
 * The binder vendor hooks run from tracepoints with preemption disabled,
 * so the handlers only resolve the task information they need and call
 * millet_report(), which defers the netlink send to a workqueue.
 *
 * Copyright (c) Xiaomi Technologies Co., Ltd. 2019-2020.
 */

#define pr_fmt(fmt) "millet-binder: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <trace/hooks/binder.h>

#include "../../android/binder_internal.h"
#include "millet.h"

static struct hlist_head *millet_binder_hhead;
static struct mutex *millet_binder_lock;

/*
 * Best-effort query of a binder process state.  Called from the netlink
 * receive path (process context) with binder_procs_lock held, so the
 * proc and its threads are stable.  t->to_thread is read without t->lock
 * on purpose: binder takes t->lock before inner_lock elsewhere, and this
 * is only a statistic.
 */
static bool millet_binder_proc_busy(struct binder_proc *proc, int *pid,
				    int *tid, enum BINDER_STAT *reason)
{
	struct binder_transaction *t;
	struct binder_thread *thread;
	struct rb_node *n;

	spin_lock(&proc->inner_lock);
	if (!list_empty(&proc->todo)) {
		if (proc->tsk) {
			*pid = task_pid_nr(proc->tsk);
			*tid = proc->tsk->pid;
		}
		*reason = BINDER_PROC_IN_BUSY;
		goto busy;
	}

	for (n = rb_first(&proc->threads); n; n = rb_next(n)) {
		thread = rb_entry(n, struct binder_thread, rb_node);
		if (!thread->task)
			continue;

		if (!list_empty(&thread->todo)) {
			*pid = task_tgid_nr(thread->task);
			*tid = thread->pid;
			*reason = BINDER_THREAD_IN_BUSY;
			goto busy;
		}

		t = READ_ONCE(thread->transaction_stack);
		if (t && READ_ONCE(t->to_thread) == thread) {
			*pid = task_tgid_nr(thread->task);
			*tid = thread->pid;
			*reason = BINDER_IN_TRANSACTION;
			goto busy;
		}
	}

	spin_unlock(&proc->inner_lock);
	return false;

busy:
	spin_unlock(&proc->inner_lock);
	return true;
}

void millet_binder_st_recv(const struct millet_userconf *msg, unsigned int len)
{
	struct binder_proc *proc;
	struct millet_data data;
	struct hlist_head *hhead;
	struct mutex *lock;
	int uid = msg->mod.u_priv.binder_st.uid;
	int pid = 0, tid = 0;
	enum BINDER_STAT reason = BINDER_IN_IDLE;

	hhead = READ_ONCE(millet_binder_hhead);
	lock = READ_ONCE(millet_binder_lock);
	if (hhead && lock) {
		mutex_lock(lock);
		hlist_for_each_entry(proc, hhead, proc_node) {
			if (!proc->tsk ||
			    (int)task_uid(proc->tsk).val != uid)
				continue;
			if (millet_binder_proc_busy(proc, &pid, &tid,
						    &reason))
				break;
		}
		mutex_unlock(lock);
	}

	memset(&data, 0, sizeof(data));
	data.msg_type = MSG_TO_USER;
	data.uid = uid;
	data.mod.k_priv.binder.stat.pid = pid;
	data.mod.k_priv.binder.stat.tid = tid;
	data.mod.k_priv.binder.stat.reason = reason;
	millet_report(BINDER_ST_TYPE, &data);
}

static void millet_binder_report(enum BINDER_EXTRA_MSG kind,
				 struct task_struct *dst, struct task_struct *src,
				 int caller_tid, bool oneway, unsigned int code)
{
	struct millet_data data;

	if (!dst || !millet_task_frozen(dst))
		return;

	memset(&data, 0, sizeof(data));
	data.msg_type = MSG_TO_USER;
	data.pri[0] = kind;
	data.uid = task_uid(dst).val;
	data.mod.k_priv.binder.trans.dst_pid = task_pid_nr(dst);
	data.mod.k_priv.binder.trans.caller_tid = caller_tid;
	data.mod.k_priv.binder.trans.tf_oneway = oneway;
	data.mod.k_priv.binder.trans.code = code;
	if (src) {
		data.mod.k_priv.binder.trans.caller_pid = task_pid_nr(src);
		data.mod.k_priv.binder.trans.caller_uid = task_uid(src).val;
	}
	millet_report(BINDER_TYPE, &data);
}

static void millet_binder_trans_hook(void *data, struct binder_proc *target_proc,
				     struct binder_proc *proc,
				     struct binder_thread *thread,
				     struct binder_transaction_data *tr)
{
	struct task_struct *dst, *src;

	if (!target_proc || !proc || !target_proc->tsk || !proc->tsk || !tr)
		return;

	dst = target_proc->tsk;
	src = proc->tsk;
	if (task_uid(dst).val <= (uid_t)frozen_uid_min ||
	    task_pid_nr(dst) == task_pid_nr(src))
		return;

	millet_binder_report(BINDER_TRANS, dst, src,
			     thread ? thread->pid : 0,
			     !!(tr->flags & TF_ONE_WAY), tr->code);
}

static void millet_binder_reply_hook(void *data, struct binder_proc *target_proc,
				     struct binder_proc *proc,
				     struct binder_thread *thread,
				     struct binder_transaction_data *tr)
{
	if (!target_proc || !proc || !target_proc->tsk || !proc->tsk || !tr)
		return;

	if (task_uid(target_proc->tsk).val > (uid_t)frozen_uid_min)
		return;

	millet_binder_report(BINDER_REPLY, target_proc->tsk, proc->tsk,
			     thread ? thread->pid : 0,
			     !!(tr->flags & TF_ONE_WAY), tr->code);
}

/*
 * Called when a binder thread is about to sleep waiting for work.
 *
 * binder_wait_for_work() releases proc->inner_lock before calling this
 * hook (see the comment there), so spin_trylock(&t->lock) cannot invert
 * the t->lock -> inner_lock order used by binder core.  A trylock keeps
 * this safe even though tracepoint callbacks run with preemption
 * disabled.
 */
static void millet_binder_wait_hook(void *data, bool do_proc_work,
				    struct binder_thread *thread,
				    struct binder_proc *proc)
{
	struct binder_transaction *t;
	struct task_struct *dst;
	unsigned int flags, code;
	int tid;

	if (!thread || !proc)
		return;

	t = READ_ONCE(thread->transaction_stack);
	if (!t || !spin_trylock(&t->lock))
		return;

	dst = t->to_proc ? READ_ONCE(t->to_proc->tsk) : NULL;
	if (dst)
		get_task_struct(dst);
	flags = t->flags;
	code = t->code;
	tid = thread->pid;
	spin_unlock(&t->lock);

	if (!dst)
		return;

	if (task_uid(dst).val <= (uid_t)frozen_uid_min &&
	    (!proc->tsk || task_pid_nr(dst) != task_pid_nr(proc->tsk)))
		millet_binder_report(BINDER_THREAD_HAS_WORK, dst, proc->tsk,
				     tid, !!(flags & TF_ONE_WAY), code);

	put_task_struct(dst);
}

static void millet_binder_alloc_hook(void *data, size_t size,
				     struct binder_alloc *alloc, int is_async)
{
	struct binder_proc *proc;
	struct task_struct *dst;

	if (!alloc || !is_async)
		return;

	if (alloc->free_async_space >=
		    (size_t)MILLET_WARN_AHEAD_MSGS *
			    (size + sizeof(struct binder_buffer)) &&
	    alloc->free_async_space >= binder_warn_ahead_space)
		return;

	proc = container_of(alloc, struct binder_proc, alloc);
	dst = READ_ONCE(proc->tsk);
	if (!dst)
		return;

	millet_binder_report(BINDER_BUFF_WARN, dst, current, current->pid,
			     false, 0);
}

static void millet_binder_preset_hook(void *data, struct hlist_head *hhead,
				      struct mutex *lock)
{
	WRITE_ONCE(millet_binder_hhead, hhead);
	WRITE_ONCE(millet_binder_lock, lock);
}

static int __init millet_binder_init(void)
{
	int ret = 0;

	ret |= register_trace_android_vh_binder_trans(millet_binder_trans_hook, NULL);
	ret |= register_trace_android_vh_binder_reply(millet_binder_reply_hook, NULL);
	ret |= register_trace_android_vh_binder_wait_for_work(millet_binder_wait_hook, NULL);
	ret |= register_trace_android_vh_binder_alloc_new_buf_locked(millet_binder_alloc_hook, NULL);
	ret |= register_trace_android_vh_binder_preset(millet_binder_preset_hook, NULL);

	if (ret)
		pr_err("failed to register hooks: %d\n", ret);

	return 0;
}
module_init(millet_binder_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mi frozen framework binder events");
