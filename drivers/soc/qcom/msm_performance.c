// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/moduleparam.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/sched/core_ctl.h>

/*
 * Sched provides the load data once per window; aggregate POLL_INT windows
 * before publishing it to userspace.
 */
#define POLL_INT 25

enum cpu_clusters {
	MIN = 0,
	MID = 1,
	MAX = 2,
	CLUSTER_MAX
};

/* To handle cpufreq min/max request */
struct cpu_status {
	unsigned int min;
	unsigned int max;
};
static DEFINE_PER_CPU(struct cpu_status, msm_perf_cpu_stats);

struct events {
	wait_queue_head_t wait;
	raw_spinlock_t cpu_hotplug_lock;
	bool cpu_hotplug;
	bool init_success;
};
static struct events events_group;
static struct task_struct *events_notify_thread;

static unsigned int aggr_big_nr;
static unsigned int aggr_top_load;
static unsigned int top_load[CLUSTER_MAX];
static unsigned int curr_cap[CLUSTER_MAX];

/*******************************sysfs start************************************/
/*
 * Parse a "cpu:value" pair list into @req. Nothing outside @req is touched,
 * so a malformed request can never leave the per-CPU limits half updated.
 *
 * Returns the number of pairs stored in @req, or a negative errno.
 */
static int msm_perf_parse_request(const char *buf, unsigned int (*req)[2],
				  int max_pairs)
{
	const char *p = buf;
	int nr = 0;

	while (*p) {
		unsigned int cpu, val;

		if (sscanf(p, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;

		if (cpu >= nr_cpu_ids)
			break;

		if (cpu_possible(cpu) && nr < max_pairs) {
			req[nr][0] = cpu;
			req[nr][1] = val;
			nr++;
		}

		p = strchr(p, ' ');
		if (!p)
			break;
		p++;
	}

	return nr;
}

/*
 * Store @val for @cpu and make every policy it belongs to re-evaluate its
 * limits. The limits are only a per-CPU record; the value that actually
 * reaches cpufreq is derived in msm_perf_adjust_notify().
 */
static int msm_perf_set_limits(const char *buf, bool is_min)
{
	unsigned int (*req)[2];
	cpumask_t mask;
	int nr, i, j;

	req = kmalloc_array(nr_cpu_ids, sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	nr = msm_perf_parse_request(buf, req, nr_cpu_ids);
	if (nr < 0) {
		kfree(req);
		return nr;
	}

	cpumask_clear(&mask);
	for (i = 0; i < nr; i++) {
		unsigned int cpu = req[i][0];
		struct cpu_status *st = &per_cpu(msm_perf_cpu_stats, cpu);

		if (is_min)
			WRITE_ONCE(st->min, req[i][1]);
		else
			WRITE_ONCE(st->max, req[i][1]);

		cpumask_set_cpu(cpu, &mask);
	}
	kfree(req);

	/*
	 * Since on synchronous systems the policy is shared amongst multiple
	 * CPUs, only one CPU per policy needs to trigger the update. Offline
	 * CPUs are skipped: their record is picked up by the ADJUST notifier
	 * once they come back, and msm_perf_adjust_notify() clamps it then.
	 */
	get_online_cpus();
	for_each_cpu(i, &mask) {
		struct cpufreq_policy policy;

		if (!cpu_online(i) || cpufreq_get_policy(&policy, i))
			continue;

		cpufreq_update_policy(i);

		for_each_cpu(j, policy.related_cpus)
			cpumask_clear_cpu(j, &mask);
	}
	put_online_cpus();

	return 0;
}

static int msm_perf_get_limits(char *buf, bool is_min)
{
	int cnt = 0, cpu;

	for_each_present_cpu(cpu) {
		unsigned int val;

		if (cnt >= PAGE_SIZE)
			break;

		if (is_min)
			val = READ_ONCE(per_cpu(msm_perf_cpu_stats, cpu).min);
		else
			val = READ_ONCE(per_cpu(msm_perf_cpu_stats, cpu).max);

		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%d:%u ", cpu, val);
	}

	if (cnt < PAGE_SIZE)
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");

	return cnt;
}

static int set_cpu_min_freq(const char *buf, const struct kernel_param *kp)
{
	return msm_perf_set_limits(buf, true);
}

static int get_cpu_min_freq(char *buf, const struct kernel_param *kp)
{
	return msm_perf_get_limits(buf, true);
}

static const struct kernel_param_ops param_ops_cpu_min_freq = {
	.set = set_cpu_min_freq,
	.get = get_cpu_min_freq,
};
module_param_cb(cpu_min_freq, &param_ops_cpu_min_freq, NULL, 0644);

static int set_cpu_max_freq(const char *buf, const struct kernel_param *kp)
{
	return msm_perf_set_limits(buf, false);
}

static int get_cpu_max_freq(char *buf, const struct kernel_param *kp)
{
	return msm_perf_get_limits(buf, false);
}

static const struct kernel_param_ops param_ops_cpu_max_freq = {
	.set = set_cpu_max_freq,
	.get = get_cpu_max_freq,
};
module_param_cb(cpu_max_freq, &param_ops_cpu_max_freq, NULL, 0644);

static struct kobject *events_kobj;

static ssize_t show_cpu_hotplug(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "\n");
}
static struct kobj_attribute cpu_hotplug_attr =
__ATTR(cpu_hotplug, 0444, show_cpu_hotplug, NULL);

static struct attribute *events_attrs[] = {
	&cpu_hotplug_attr.attr,
	NULL,
};

static struct attribute_group events_attr_group = {
	.attrs = events_attrs,
};

static ssize_t show_big_nr(struct kobject *kobj,
			   struct kobj_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", aggr_big_nr);
}

static struct kobj_attribute big_nr_attr =
__ATTR(aggr_big_nr, 0444, show_big_nr, NULL);

static ssize_t show_top_load(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", aggr_top_load);
}

static struct kobj_attribute top_load_attr =
__ATTR(aggr_top_load, 0444, show_top_load, NULL);


static ssize_t show_top_load_cluster(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u %u %u\n",
					top_load[MIN], top_load[MID],
					top_load[MAX]);
}

static struct kobj_attribute cluster_top_load_attr =
__ATTR(top_load_cluster, 0444, show_top_load_cluster, NULL);

static ssize_t show_curr_cap_cluster(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u %u %u\n",
					curr_cap[MIN], curr_cap[MID],
					curr_cap[MAX]);
}

static struct kobj_attribute cluster_curr_cap_attr =
__ATTR(curr_cap_cluster, 0444, show_curr_cap_cluster, NULL);

static struct attribute *notify_attrs[] = {
	&big_nr_attr.attr,
	&top_load_attr.attr,
	&cluster_top_load_attr.attr,
	&cluster_curr_cap_attr.attr,
	NULL,
};

static struct attribute_group notify_attr_group = {
	.attrs = notify_attrs,
};
static struct kobject *notify_kobj;

/*******************************sysfs ends************************************/

static int perf_adjust_notify(struct notifier_block *nb, unsigned long val,
							void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int min = 0, max = UINT_MAX;
	int cpu;

	if (val != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	/*
	 * Aggregate over the whole policy. Userspace is free to address a
	 * single CPU, but the limit is shared by every CPU of the policy, so
	 * reading only policy->cpu would drop the request for a sibling.
	 */
	for_each_cpu(cpu, policy->related_cpus) {
		struct cpu_status *st = &per_cpu(msm_perf_cpu_stats, cpu);

		min = max(min, READ_ONCE(st->min));
		max = min(max, READ_ONCE(st->max));
	}

	/*
	 * A floor above the current ceiling must not lift the ceiling.
	 * cpufreq_verify_within_limits() raises policy->max to min when
	 * policy->max < min, which would silently defeat a thermal cap or a
	 * scaling_max_freq request and pin the cluster at its highest OPP.
	 */
	if (min > policy->cpuinfo.max_freq)
		min = policy->cpuinfo.max_freq;
	if (min > policy->max)
		min = policy->max;
	if (max < min)
		max = min;

	pr_debug("msm_perf: policy%u before: %u:%u, request %u:%u kHz\n",
		 policy->cpu, policy->min, policy->max, min, max);

	cpufreq_verify_within_limits(policy, min, max);

	pr_debug("msm_perf: policy%u after: %u:%u kHz\n",
		 policy->cpu, policy->min, policy->max);

	return NOTIFY_OK;
}

static struct notifier_block perf_cpufreq_nb = {
	.notifier_call = perf_adjust_notify,
};

static int hotplug_notify(unsigned int cpu)
{
	unsigned long flags;

	if (!events_group.init_success)
		return 0;

	/*
	 * Runs from the CPU hotplug path, which must not sleep. A rt_mutex
	 * based spinlock_t would do exactly that on PREEMPT_RT, so keep this
	 * a raw lock: it only guards a single flag.
	 */
	raw_spin_lock_irqsave(&events_group.cpu_hotplug_lock, flags);
	events_group.cpu_hotplug = true;
	raw_spin_unlock_irqrestore(&events_group.cpu_hotplug_lock, flags);

	wake_up_interruptible(&events_group.wait);

	return 0;
}

static int events_notify_userspace(void *data)
{
	unsigned long flags;

	while (!kthread_should_stop()) {
		bool notify_change;

		wait_event_interruptible(events_group.wait,
					 READ_ONCE(events_group.cpu_hotplug) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		raw_spin_lock_irqsave(&events_group.cpu_hotplug_lock, flags);
		notify_change = events_group.cpu_hotplug;
		events_group.cpu_hotplug = false;
		raw_spin_unlock_irqrestore(&events_group.cpu_hotplug_lock, flags);

		if (notify_change && events_kobj)
			sysfs_notify(events_kobj, NULL, "cpu_hotplug");
	}

	return 0;
}

static int init_notify_group(void)
{
	struct kobject *module_kobj;
	int ret;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("msm_perf: Couldn't find module kobject\n");
		return -ENOENT;
	}

	notify_kobj = kobject_create_and_add("notify", module_kobj);
	kobject_put(module_kobj);
	if (!notify_kobj) {
		pr_err("msm_perf: Failed to add notify_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(notify_kobj, &notify_attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create notify sysfs\n");
		kobject_put(notify_kobj);
		notify_kobj = NULL;
		return ret;
	}

	return 0;
}

static int init_events_group(void)
{
	struct kobject *module_kobj;
	int ret;

	init_waitqueue_head(&events_group.wait);
	raw_spin_lock_init(&events_group.cpu_hotplug_lock);

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("msm_perf: Couldn't find module kobject\n");
		return -ENOENT;
	}

	events_kobj = kobject_create_and_add("events", module_kobj);
	kobject_put(module_kobj);
	if (!events_kobj) {
		pr_err("msm_perf: Failed to add events_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(events_kobj, &events_attr_group);
	if (ret) {
		pr_err("msm_perf: Failed to create events sysfs\n");
		kobject_put(events_kobj);
		events_kobj = NULL;
		return ret;
	}

	events_notify_thread = kthread_run(events_notify_userspace,
					NULL, "msm_perf:events_notify");
	if (IS_ERR(events_notify_thread)) {
		ret = PTR_ERR(events_notify_thread);
		events_notify_thread = NULL;
		return ret;
	}

	events_group.init_success = true;

	return 0;
}

static void nr_notify_userspace(struct work_struct *work)
{
	if (!notify_kobj)
		return;

	sysfs_notify(notify_kobj, NULL, "aggr_top_load");
	sysfs_notify(notify_kobj, NULL, "aggr_big_nr");
	sysfs_notify(notify_kobj, NULL, "top_load_cluster");
	sysfs_notify(notify_kobj, NULL, "curr_cap_cluster");
}
static DECLARE_WORK(sysfs_notify_work, nr_notify_userspace);

static DEFINE_RAW_SPINLOCK(msm_perf_aggr_lock);

static int msm_perf_core_ctl_notify(struct notifier_block *nb,
					unsigned long unused,
					void *data)
{
	static unsigned int tld, nrb, i;
	static unsigned int top_ld[CLUSTER_MAX], curr_cp[CLUSTER_MAX];
	struct core_ctl_notif_data *d = data;
	unsigned long flags;
	bool notify = false;
	int cluster;

	raw_spin_lock_irqsave(&msm_perf_aggr_lock, flags);

	nrb += d->nr_big;
	tld += d->coloc_load_pct;
	for (cluster = 0; cluster < CLUSTER_MAX; cluster++) {
		top_ld[cluster] += d->ta_util_pct[cluster];
		curr_cp[cluster] += d->cur_cap_pct[cluster];
	}

	if (++i == POLL_INT) {
		aggr_big_nr = ((nrb % POLL_INT) ? 1 : 0) + nrb / POLL_INT;
		aggr_top_load = tld / POLL_INT;
		for (cluster = 0; cluster < CLUSTER_MAX; cluster++) {
			top_load[cluster] = top_ld[cluster] / POLL_INT;
			curr_cap[cluster] = curr_cp[cluster] / POLL_INT;
			top_ld[cluster] = 0;
			curr_cp[cluster] = 0;
		}
		/* reset counters */
		tld = 0;
		nrb = 0;
		i = 0;
		notify = true;
	}

	raw_spin_unlock_irqrestore(&msm_perf_aggr_lock, flags);

	if (notify)
		schedule_work(&sysfs_notify_work);

	return NOTIFY_OK;
}

static struct notifier_block msm_perf_nb = {
	.notifier_call = msm_perf_core_ctl_notify
};

static bool core_ctl_register;
static int set_core_ctl_register(const char *buf, const struct kernel_param *kp)
{
	int ret;
	bool old_val = core_ctl_register;

	ret = param_set_bool(buf, kp);
	if (ret < 0)
		return ret;

	if (core_ctl_register == old_val)
		return 0;

	if (core_ctl_register)
		core_ctl_notifier_register(&msm_perf_nb);
	else
		core_ctl_notifier_unregister(&msm_perf_nb);

	return 0;
}

static const struct kernel_param_ops param_ops_cc_register = {
	.set = set_core_ctl_register,
	.get = param_get_bool,
};
module_param_cb(core_ctl_register, &param_ops_cc_register,
		&core_ctl_register, 0644);

static int __init msm_performance_init(void)
{
	unsigned int cpu;
	int rc;

	/*
	 * Seed the per-CPU records before the notifier can run. A zero max
	 * would make cpufreq_verify_within_limits() clamp the policy to 0 as
	 * soon as anything triggers CPUFREQ_ADJUST.
	 */
	for_each_present_cpu(cpu) {
		per_cpu(msm_perf_cpu_stats, cpu).min = 0;
		per_cpu(msm_perf_cpu_stats, cpu).max = UINT_MAX;
	}

	rc = cpufreq_register_notifier(&perf_cpufreq_nb,
				       CPUFREQ_POLICY_NOTIFIER);
	if (rc) {
		pr_err("msm_perf: failed to register cpufreq notifier: %d\n",
		       rc);
		return rc;
	}

	rc = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE,
		"msm_performance_cpu_hotplug",
		hotplug_notify,
		NULL);
	if (rc) {
		pr_err("msm_perf: failed to install hotplug callback: %d\n",
		       rc);
		cpufreq_unregister_notifier(&perf_cpufreq_nb,
					    CPUFREQ_POLICY_NOTIFIER);
		return rc;
	}

	/*
	 * Both groups only publish statistics to userspace; keep the cpufreq
	 * limits working even if the sysfs side fails to come up.
	 */
	if (init_events_group())
		pr_err("msm_perf: events group unavailable\n");

	if (init_notify_group())
		pr_err("msm_perf: notify group unavailable\n");

	return 0;
}
late_initcall(msm_performance_init);
