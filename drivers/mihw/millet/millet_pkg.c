// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mi frozen framework (millet) - package events
 *
 * Userspace registers UIDs whose next incoming TCP connection should be
 * reported.  The netfilter hook runs in softirq context, so the UID table
 * is protected by a raw spinlock and the report is queued.
 *
 * Copyright (c) Xiaomi Technologies Co., Ltd. 2019-2020.
 */

#define pr_fmt(fmt) "millet-pkg: " fmt

#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/sock.h>

#include "millet.h"

static int millet_pkg_uids[MILLET_PKG_MAX_UIDS];
static DEFINE_RAW_SPINLOCK(millet_pkg_lock);

static bool millet_pkg_consume_uid(uid_t uid)
{
	unsigned long flags;
	bool found = false;
	int i;

	raw_spin_lock_irqsave(&millet_pkg_lock, flags);
	for (i = 0; i < MILLET_PKG_MAX_UIDS; i++) {
		if ((uid_t)millet_pkg_uids[i] == uid) {
			millet_pkg_uids[i] = 0;
			found = true;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&millet_pkg_lock, flags);
	return found;
}

void millet_pkg_recv(const struct millet_userconf *msg, unsigned int len)
{
	unsigned long flags;
	uid_t uid = msg->mod.u_priv.pkg.uid;
	int i;

	switch (msg->mod.u_priv.pkg.cmd) {
	case ADD_UID:
		raw_spin_lock_irqsave(&millet_pkg_lock, flags);
		for (i = 0; i < MILLET_PKG_MAX_UIDS; i++) {
			if ((uid_t)millet_pkg_uids[i] == uid)
				break;
			if (!millet_pkg_uids[i]) {
				millet_pkg_uids[i] = uid;
				break;
			}
		}
		raw_spin_unlock_irqrestore(&millet_pkg_lock, flags);
		if (i == MILLET_PKG_MAX_UIDS)
			pr_err("uid table full, drop %u\n", uid);
		break;
	case DEL_UID:
		raw_spin_lock_irqsave(&millet_pkg_lock, flags);
		for (i = 0; i < MILLET_PKG_MAX_UIDS; i++) {
			if ((uid_t)millet_pkg_uids[i] == uid) {
				millet_pkg_uids[i] = 0;
				break;
			}
		}
		raw_spin_unlock_irqrestore(&millet_pkg_lock, flags);
		break;
	case CLEAR_ALL_UID:
		raw_spin_lock_irqsave(&millet_pkg_lock, flags);
		memset(millet_pkg_uids, 0, sizeof(millet_pkg_uids));
		raw_spin_unlock_irqrestore(&millet_pkg_lock, flags);
		break;
	default:
		break;
	}
}

/*
 * Only the inode UID is needed and this runs in softirq context, so do
 * not use sock_i_uid(): its callback_lock is a sleeping lock on RT.
 */
static uid_t millet_sock_uid(struct sock *sk)
{
	if (sk && sk->sk_socket)
		return SOCK_INODE(sk->sk_socket)->i_uid.val;
	return 0;
}

static unsigned int millet_pkg_hook(void *priv, struct sk_buff *skb,
				    const struct nf_hook_state *state)
{
	struct millet_data data;
	unsigned short frag_off = 0;
	unsigned int thoff = 0;
	struct sock *sk;
	uid_t uid;
	int protocol;

	switch (state->pf) {
	case NFPROTO_IPV4:
		protocol = ip_hdr(skb)->protocol;
		break;
	case NFPROTO_IPV6:
		protocol = ipv6_find_hdr(skb, &thoff, -1, &frag_off, NULL);
		break;
	default:
		return NF_ACCEPT;
	}

	if (protocol != IPPROTO_TCP)
		return NF_ACCEPT;

	sk = skb_to_full_sk(skb);
	if (!sk || !sk_fullsock(sk))
		return NF_ACCEPT;

	uid = millet_sock_uid(sk);
	if (uid < UID_MIN_VALUE)
		return NF_ACCEPT;

	if (!millet_pkg_consume_uid(uid))
		return NF_ACCEPT;

	memset(&data, 0, sizeof(data));
	data.msg_type = MSG_TO_USER;
	data.uid = uid;
	data.mod.k_priv.pkg.pkg_owner = uid;
	millet_report(PKG_TYPE, &data);

	return NF_ACCEPT;
}

static const struct nf_hook_ops millet_pkg_nf_ops[] = {
	{
		.hook = millet_pkg_hook,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_LOCAL_IN,
		.priority = NF_IP_PRI_SELINUX_LAST + 1,
	},
	{
		.hook = millet_pkg_hook,
		.pf = NFPROTO_IPV6,
		.hooknum = NF_INET_LOCAL_IN,
		.priority = NF_IP6_PRI_SELINUX_LAST + 1,
	},
};

static int __init millet_pkg_init(void)
{
	int ret;

	ret = nf_register_net_hooks(&init_net, millet_pkg_nf_ops,
				    ARRAY_SIZE(millet_pkg_nf_ops));
	if (ret) {
		pr_err("failed to register netfilter hooks: %d\n", ret);
		return ret;
	}

	pr_info("initialized\n");
	return 0;
}
module_init(millet_pkg_init);

static void __exit millet_pkg_exit(void)
{
	nf_unregister_net_hooks(&init_net, millet_pkg_nf_ops,
				ARRAY_SIZE(millet_pkg_nf_ops));
}
module_exit(millet_pkg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mi frozen framework package events");
