// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mi frozen framework (millet) - core
 *
 * Reports freezer, binder, signal and package events to a userspace
 * monitor over NETLINK_MILLET.
 *
 * All report sites are reachable from atomic context (android_vh_*
 * tracepoints run with preemption disabled, netfilter hooks run in
 * softirq context and the signal hook may run with siglock held).  They
 * therefore only build an skb with GFP_ATOMIC and queue it; the actual
 * nlmsg_unicast() runs from a workqueue in process context.
 *
 * Copyright (c) Xiaomi Technologies Co., Ltd. 2019-2020.
 */

#define pr_fmt(fmt) "millet: " fmt

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <net/sock.h>

#include "millet.h"

unsigned int millet_debug;
int frozen_uid_min = 10000;
unsigned long binder_warn_ahead_space = MILLET_WARN_AHEAD_SPACE;

module_param(millet_debug, uint, 0644);
module_param(frozen_uid_min, int, 0644);
module_param(binder_warn_ahead_space, ulong, 0644);

static struct sock *millet_sock;
static atomic_t millet_ready = ATOMIC_INIT(0);
static atomic_t millet_ports[MILLET_TYPES_NUM];

static const u8 millet_monitor[MILLET_TYPES_NUM] = {
	[SIG_TYPE] = SIG_TYPE,
	[BINDER_TYPE] = BINDER_TYPE,
	[BINDER_ST_TYPE] = BINDER_TYPE,
	[PKG_TYPE] = PKG_TYPE,
	[HANDSHK_TYPE] = SIG_TYPE,
};

static const char * const millet_names[MILLET_TYPES_NUM] = {
	[O_TYPE] = "NULL",
	[SIG_TYPE] = "SIG",
	[BINDER_TYPE] = "BINDER",
	[BINDER_ST_TYPE] = "BINDER_STAT",
	[MEM_TYPE] = "MEM",
	[PKG_TYPE] = "PKG",
	[HANDSHK_TYPE] = "HANDSHK",
};

struct millet_stats {
	atomic64_t send_suc;
	atomic64_t send_fail;
	atomic64_t runtime_ns;
};

static struct millet_stats millet_stats[MILLET_TYPES_NUM];

struct millet_tx_msg {
	struct list_head node;
	struct sk_buff *skb;
	struct sock *sock;
	u32 portid;
};

static LIST_HEAD(millet_tx_list);
static DEFINE_RAW_SPINLOCK(millet_tx_lock);
static atomic_t millet_tx_len = ATOMIC_INIT(0);

static void millet_tx_work_fn(struct work_struct *work)
{
	struct millet_tx_msg *msg, *tmp;
	LIST_HEAD(batch);
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&millet_tx_lock, flags);
	list_splice_init(&millet_tx_list, &batch);
	raw_spin_unlock_irqrestore(&millet_tx_lock, flags);

	list_for_each_entry_safe(msg, tmp, &batch, node) {
		list_del(&msg->node);
		ret = nlmsg_unicast(msg->sock, msg->skb, msg->portid);
		if (unlikely(ret < 0 && millet_debug))
			pr_err("tx to port %u failed: %d\n", msg->portid, ret);
		kfree(msg);
		atomic_dec(&millet_tx_len);
	}
}

static DECLARE_WORK(millet_tx_work, millet_tx_work_fn);

/* Never expose kernel addresses to userspace. */
static void millet_scrub_pointers(struct millet_data *data)
{
	data->mod.k_priv.sig.caller_task = NULL;
	data->mod.k_priv.sig.killed_task = NULL;
	data->mod.k_priv.binder.stat.task = NULL;
	data->mod.k_priv.binder.trans.src_task = NULL;
	data->mod.k_priv.binder.trans.dst_task = NULL;
}

int millet_report(enum MILLET_TYPE type, const struct millet_data *data)
{
	struct millet_tx_msg *msg;
	struct millet_data *payload;
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	struct timespec64 ts;
	enum MILLET_TYPE monitor;
	unsigned long flags;
	ktime_t start;
	u32 portid;
	int ret = -EINVAL;

	if (unlikely(!atomic_read(&millet_ready)))
		return -ENODEV;
	if (!TYPE_VALID(type) || !data || !MSG_VALID(data->msg_type))
		goto fail;

	monitor = millet_monitor[type];
	if (!TYPE_VALID(monitor))
		monitor = type;

	portid = (u32)atomic_read(&millet_ports[monitor]);
	if (!portid)
		goto fail;

	start = ktime_get();
	if (atomic_inc_return(&millet_tx_len) > MILLET_TX_MAX) {
		atomic_dec(&millet_tx_len);
		ret = -ENOBUFS;
		goto fail;
	}

	skb = nlmsg_new(sizeof(*payload), GFP_ATOMIC);
	if (!skb) {
		atomic_dec(&millet_tx_len);
		ret = -ENOMEM;
		goto fail;
	}

	nlh = nlmsg_put(skb, 0, 0, 0, sizeof(*payload), 0);
	if (!nlh) {
		kfree_skb(skb);
		atomic_dec(&millet_tx_len);
		ret = -ENOMEM;
		goto fail;
	}

	payload = nlmsg_data(nlh);
	memcpy(payload, data, sizeof(*payload));
	payload->owner = type;
	payload->monitor = monitor;
	payload->src_port = MILLET_KERNEL_ID;
	payload->dst_port = MILLET_USER_ID;
	millet_scrub_pointers(payload);
	ktime_get_ts64(&ts);
	payload->tm.sec = ts.tv_sec;
	payload->tm.nsec = ts.tv_nsec;

	msg = kmalloc(sizeof(*msg), GFP_ATOMIC);
	if (!msg) {
		kfree_skb(skb);
		atomic_dec(&millet_tx_len);
		ret = -ENOMEM;
		goto fail;
	}
	msg->skb = skb;
	msg->sock = millet_sock;
	msg->portid = portid;

	raw_spin_lock_irqsave(&millet_tx_lock, flags);
	list_add_tail(&msg->node, &millet_tx_list);
	raw_spin_unlock_irqrestore(&millet_tx_lock, flags);
	schedule_work(&millet_tx_work);

	atomic64_inc(&millet_stats[type].send_suc);
	atomic64_add(ktime_to_ns(ktime_sub(ktime_get(), start)),
		     &millet_stats[type].runtime_ns);
	return 0;

fail:
	if (TYPE_VALID(type))
		atomic64_inc(&millet_stats[type].send_fail);
	return ret;
}

static void millet_hs_recv(const struct millet_userconf *msg, unsigned int len)
{
	struct millet_data data;

	memset(&data, 0, sizeof(data));
	data.msg_type = MSG_TO_USER;
	millet_report(HANDSHK_TYPE, &data);
}

static void millet_dispatch_recv(const struct millet_userconf *msg,
				 unsigned int len)
{
	switch (msg->owner) {
	case BINDER_ST_TYPE:
		millet_binder_st_recv(msg, len);
		break;
	case PKG_TYPE:
		millet_pkg_recv(msg, len);
		break;
	case HANDSHK_TYPE:
		millet_hs_recv(msg, len);
		break;
	default:
		break;
	}
}

static void millet_recv(struct sk_buff *skb)
{
	const struct millet_userconf *msg;
	struct nlmsghdr *nlh;
	struct millet_data data;
	unsigned int len = sizeof(*msg);
	uid_t uid;

	if (!skb)
		return;

	uid = NETLINK_CREDS(skb)->uid.val;
	if (uid > 1000) {
		pr_err("uid %u not allowed\n", uid);
		return;
	}

	if (unlikely(skb->len < NLMSG_HDRLEN)) {
		pr_err("short message (%u)\n", skb->len);
		return;
	}

	nlh = nlmsg_hdr(skb);
	if (unlikely(nlh->nlmsg_len < NLMSG_SPACE(len) ||
		     skb->len < NLMSG_SPACE(len))) {
		pr_err("bad message length (len %u, nlmsg_len %u, need %u)\n",
		       skb->len, nlh->nlmsg_len, NLMSG_SPACE(len));
		return;
	}

	msg = NLMSG_DATA(nlh);
	if (unlikely(msg->src_port != MILLET_USER_ID ||
		     msg->dst_port != MILLET_KERNEL_ID)) {
		pr_err("invalid ports %#lx -> %#lx\n",
		       msg->src_port, msg->dst_port);
		return;
	}
	if (unlikely(!TYPE_VALID(msg->owner))) {
		pr_err("invalid owner %d\n", msg->owner);
		return;
	}

	if (millet_debug)
		pr_info("recv type %d msg %d port %u\n", msg->owner,
			msg->msg_type, nlh->nlmsg_pid);

	switch (msg->msg_type) {
	case LOOPBACK_MSG:
		atomic_set(&millet_ports[msg->owner], nlh->nlmsg_pid);
		memset(&data, 0, sizeof(data));
		data.msg_type = LOOPBACK_MSG;
		millet_report(msg->owner, &data);
		break;
	case MSG_TO_KERN:
		millet_dispatch_recv(msg, len);
		break;
	default:
		pr_err("invalid msg_type %d\n", msg->msg_type);
		break;
	}
}

static int millet_stat_show(struct seq_file *m, void *v)
{
	unsigned long long send_suc = 0, send_fail = 0, runtime = 0;
	int i;

	for (i = O_TYPE + 1; i < MILLET_TYPES_NUM; i++) {
		u64 suc = atomic64_read(&millet_stats[i].send_suc);
		u64 fail = atomic64_read(&millet_stats[i].send_fail);
		u64 ns = atomic64_read(&millet_stats[i].runtime_ns);

		if (!suc && !fail)
			continue;

		seq_printf(m, "name: %s mod id %d:\n", millet_names[i], i);
		seq_printf(m, "send suc: %llu\n", (unsigned long long)suc);
		seq_printf(m, "send fail: %llu\n", (unsigned long long)fail);
		seq_printf(m, "runtime: %llu us\n\n",
			   (unsigned long long)(ns / 1000));

		send_suc += suc;
		send_fail += fail;
		runtime += ns;
	}

	seq_printf(m, "---------total info--------------------\n");
	seq_printf(m, "send_suc: %llu\n", send_suc);
	seq_printf(m, "send_fail: %llu\n", send_fail);
	seq_printf(m, "runtime: %llu us\n", runtime / 1000);
	return 0;
}

static int millet_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, millet_stat_show, NULL);
}

static void millet_stat_reset(void)
{
	int i;

	for (i = O_TYPE + 1; i < MILLET_TYPES_NUM; i++) {
		atomic64_set(&millet_stats[i].send_suc, 0);
		atomic64_set(&millet_stats[i].send_fail, 0);
		atomic64_set(&millet_stats[i].runtime_ns, 0);
	}
}

static ssize_t millet_stat_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	char buf[8];
	int value;

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (kstrtoint(buf, 10, &value))
		return -EINVAL;
	if (value == 1)
		millet_stat_reset();
	return count;
}

static const struct file_operations millet_stat_fops = {
	.open = millet_stat_open,
	.read = seq_read,
	.write = millet_stat_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int millet_version_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", VERSION_1_0);
	return 0;
}

static int millet_version_open(struct inode *inode, struct file *file)
{
	return single_open(file, millet_version_show, NULL);
}

static const struct file_operations millet_version_fops = {
	.open = millet_version_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init millet_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.input = millet_recv,
	};
	struct proc_dir_entry *root;

	millet_sock = netlink_kernel_create(&init_net, NETLINK_MILLET, &cfg);
	if (!millet_sock) {
		pr_err("failed to create netlink socket\n");
		return -ENOMEM;
	}

	root = proc_mkdir("millet", NULL);
	if (root) {
		proc_create("millet_stat", 0644, root, &millet_stat_fops);
		proc_create("version", 0644, root, &millet_version_fops);
	}

	atomic_set(&millet_ready, 1);
	pr_info("initialized\n");
	return 0;
}
module_init(millet_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mi frozen framework core");
