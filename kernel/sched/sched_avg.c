// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012, 2015-2019, The Linux Foundation. All rights reserved.
 */

/*
 * Scheduler hook for average runqueue determination
 */
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>
#include <linux/sched.h>
#include <linux/math64.h>

#include "sched.h"
#include "walt.h"
#include <trace/events/sched.h>

static DEFINE_PER_CPU(u64, nr_prod_sum);
static DEFINE_PER_CPU(u64, last_time);
static DEFINE_PER_CPU(u64, nr_big_prod_sum);
static DEFINE_PER_CPU(u64, nr);
static DEFINE_PER_CPU(u64, nr_max);

static DEFINE_PER_CPU(raw_spinlock_t, nr_lock) = __RAW_SPIN_LOCK_UNLOCKED(nr_lock);
static s64 last_get_time;

#define NR_THRESHOLD_PCT		15

/**
 * sched_get_nr_running_avg
 * @return: Average nr_running, iowait and nr_big_tasks value since last poll.
 *	    Returns the avg * 100 to return up to two decimal points
 *	    of accuracy.
 *
 * Obtains the average nr_running value since the last poll.
 * This function may not be called concurrently with itself
 */
void sched_get_nr_running_avg(struct sched_avg_stats *stats)
{
	int cpu;
	u64 curr_time = sched_clock();
	u64 period = curr_time - last_get_time;
	u64 tmp_nr, tmp_misfit;

	if (!period)
		return;

	/* read and reset nr_running counts */
	for_each_possible_cpu(cpu) {
		unsigned long flags;
		u64 diff;

		raw_spin_lock_irqsave(&per_cpu(nr_lock, cpu), flags);
		curr_time = sched_clock();
		diff = curr_time - per_cpu(last_time, cpu);
		BUG_ON((s64)diff < 0);

		tmp_nr = per_cpu(nr_prod_sum, cpu);
		tmp_nr += per_cpu(nr, cpu) * diff;
		tmp_nr = div64_u64((tmp_nr * 100), period);

		tmp_misfit = per_cpu(nr_big_prod_sum, cpu);
		tmp_misfit += walt_big_tasks(cpu) * diff;
		tmp_misfit = div64_u64((tmp_misfit * 100), period);

		/*
		 * NR_THRESHOLD_PCT is to make sure that the task ran
		 * at least 85% in the last window to compensate any
		 * over estimating being done.
		 */
		stats[cpu].nr = (int)div64_u64((tmp_nr + NR_THRESHOLD_PCT),
								100);
		stats[cpu].nr_misfit = (int)div64_u64((tmp_misfit +
						NR_THRESHOLD_PCT), 100);
		stats[cpu].nr_max = per_cpu(nr_max, cpu);
		stats[cpu].nr_scaled = tmp_nr;

		trace_sched_get_nr_running_avg(cpu, stats[cpu].nr,
				stats[cpu].nr_misfit, stats[cpu].nr_max,
				stats[cpu].nr_scaled);

		per_cpu(last_time, cpu) = curr_time;
		per_cpu(nr_prod_sum, cpu) = 0;
		per_cpu(nr_big_prod_sum, cpu) = 0;
		per_cpu(nr_max, cpu) = per_cpu(nr, cpu);

		raw_spin_unlock_irqrestore(&per_cpu(nr_lock, cpu), flags);
	}

	last_get_time = curr_time;

}
EXPORT_SYMBOL(sched_get_nr_running_avg);

/**
 * sched_update_nr_prod
 * @cpu: The core id of the nr running driver.
 * @delta: Adjust nr by 'delta' amount
 * @inc: Whether we are increasing or decreasing the count
 * @return: N/A
 *
 * Update average with latest nr_running value for CPU
 */
void sched_update_nr_prod(int cpu, long delta, bool inc)
{
	u64 diff;
	u64 curr_time;
	unsigned long flags, nr_running;

	raw_spin_lock_irqsave(&per_cpu(nr_lock, cpu), flags);
	nr_running = per_cpu(nr, cpu);
	curr_time = sched_clock();
	diff = curr_time - per_cpu(last_time, cpu);
	BUG_ON((s64)diff < 0);
	per_cpu(last_time, cpu) = curr_time;
	per_cpu(nr, cpu) = nr_running + (inc ? delta : -delta);

	BUG_ON((s64)per_cpu(nr, cpu) < 0);

	if (per_cpu(nr, cpu) > per_cpu(nr_max, cpu))
		per_cpu(nr_max, cpu) = per_cpu(nr, cpu);

	per_cpu(nr_prod_sum, cpu) += nr_running * diff;
	per_cpu(nr_big_prod_sum, cpu) += walt_big_tasks(cpu) * diff;
	raw_spin_unlock_irqrestore(&per_cpu(nr_lock, cpu), flags);
}
EXPORT_SYMBOL(sched_update_nr_prod);

/*
 * Returns the CPU utilization % in the last window.
 *
 */
unsigned int sched_get_cpu_util(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	u64 util;
	unsigned long capacity, flags;
	unsigned int busy;

	raw_spin_lock_irqsave(&rq->lock, flags);

	util = rq->cfs.avg.util_avg;
	capacity = capacity_orig_of(cpu);

	util = rq->prev_runnable_sum + rq->grp_time.prev_runnable_sum;
	util = div64_u64(util, sched_ravg_window >> SCHED_CAPACITY_SHIFT);

	raw_spin_unlock_irqrestore(&rq->lock, flags);

	util = (util >= capacity) ? capacity : util;
	busy = div64_ul((util * 100), capacity);
	return busy;
}
