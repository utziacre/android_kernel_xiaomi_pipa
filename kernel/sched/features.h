/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Only give sleepers 50% of their service deficit. This allows
 * them to run sooner, but does not allow tons of sleepers to
 * rip the spread apart.
 */
#define SCHED_FEAT_GENTLE_FAIR_SLEEPERS 1

/*
 * Place new tasks ahead so that they do not starve already running
 * tasks
 */
#define SCHED_FEAT_START_DEBIT 1

/*
 * Prefer to schedule the task we woke last (assuming it failed
 * wakeup-preemption), since its likely going to consume data we
 * touched, increases cache locality.
 */
#define SCHED_FEAT_NEXT_BUDDY 0

/*
 * Prefer to schedule the task that ran last (when we did
 * wake-preempt) as that likely will touch the same data, increases
 * cache locality.
 */
#define SCHED_FEAT_LAST_BUDDY 1

/*
 * Consider buddies to be cache hot, decreases the likelyness of a
 * cache buddy being migrated away, increases cache locality.
 */
#define SCHED_FEAT_CACHE_HOT_BUDDY 1

/*
 * Allow wakeup-time preemption of the current task:
 */
#define SCHED_FEAT_WAKEUP_PREEMPTION 1

#define SCHED_FEAT_HRTICK 0
#define SCHED_FEAT_DOUBLE_TICK 0
#define SCHED_FEAT_LB_BIAS 1

/*
 * Decrement CPU capacity based on time not spent running tasks
 */
#define SCHED_FEAT_NONTASK_CAPACITY 1

#ifdef CONFIG_PREEMPT_RT_FULL
#define SCHED_FEAT_TTWU_QUEUE 0

#ifdef CONFIG_PREEMPT_LAZY
#define SCHED_FEAT_PREEMPT_LAZY 1
#endif

#else

/*
 * Queue remote wakeups on the target CPU and process them
 * using the scheduler IPI. Reduces rq->lock contention/bounces.
 */
#define SCHED_FEAT_TTWU_QUEUE 0
#endif

/*
 * When doing wakeups, attempt to limit superfluous scans of the LLC domain.
 */
#define SCHED_FEAT_SIS_AVG_CPU 0
#define SCHED_FEAT_SIS_PROP 1

/*
 * SIS_UTIL: bound the idle CPU scan of select_idle_cpu() by the utilization
 * of the LLC domain instead of a fixed cost estimate. A saturated domain is
 * not scanned at all.
 *
 * Disabled on purpose: the heuristic was introduced for very large LLCs
 * where a full scan is expensive. On an 8 CPU phone the scan costs a few
 * microseconds, while skipping it can cost a whole scheduling slice of
 * runnable wait, so the trade is the wrong way round for latency. It also
 * feeds on sds->total_util, which is PELT based, while everything else in
 * this kernel (task placement, schedutil, idle prediction) uses WALT.
 */
#define SCHED_FEAT_SIS_UTIL 0

/*
 * Issue a WARN when we do multiple update_rq_clock() calls
 * in a single rq->lock section. Default disabled because the
 * annotations are not complete.
 */
#define SCHED_FEAT_WARN_DOUBLE_CLOCK 0

#if defined(CONFIG_IRQ_WORK) && defined(CONFIG_SMP)
/*
 * In order to avoid a thundering herd attack of CPUs that are
 * lowering their priorities at the same time, and there being
 * a single CPU that has an RT task that can migrate and is waiting
 * to run, where the other CPUs will try to take that CPUs
 * rq lock and possibly create a large contention, sending an
 * IPI to that CPU and let that CPU push the RT task to where
 * it should go may be a better scenario.
 *
 * This is best for PREEMPT_RT, but for non-RT it can cause issues
 * when preemption is disabled for long periods of time. Have
 * it only default enabled for PREEMPT_RT.
 */
# ifdef CONFIG_PREEMPT_RT
#define SCHED_FEAT_RT_PUSH_IPI 1
# else
#define SCHED_FEAT_RT_PUSH_IPI 0
# endif
#endif

#define SCHED_FEAT_RT_RUNTIME_SHARE 0
#define SCHED_FEAT_LB_MIN 0
#define SCHED_FEAT_ATTACH_AGE_LOAD 1

#define SCHED_FEAT_WA_IDLE 1
#define SCHED_FEAT_WA_WEIGHT 1
#define SCHED_FEAT_WA_BIAS 1

/*
 * UtilEstimation. Use estimated CPU utilization.
 */
#define SCHED_FEAT_UTIL_EST 1
#define SCHED_FEAT_UTIL_EST_FASTUP 1

/* Optional overflow diagnostics for the EEVDF weighted average. */
#ifndef SCHED_FEAT_PARANOID_AVG
#define SCHED_FEAT_PARANOID_AVG 0
#endif

/*
 * Fast pre-selection of CPU candidates for EAS.
 */
#define SCHED_FEAT_FIND_BEST_TARGET 1

/*
 * Energy aware scheduling algorithm choices:
 * EAS_PREFER_IDLE
 *   Direct tasks in a schedtune.prefer_idle=1 group through
 *   the EAS path for wakeup task placement. Otherwise, put
 *   those tasks through the mainline slow path.
 */
#define SCHED_FEAT_EAS_PREFER_IDLE 1

/*
 * Request max frequency from schedutil whenever a RT task is running.
 */
#define SCHED_FEAT_SUGOV_RT_MAX_FREQ 0

/*
 * Apply schedtune boost hold to tasks of all sched classes.
 * If enabled, schedtune will hold the boost applied to a CPU
 * for 50ms regardless of task activation - if the task is
 * still running 50ms later, the boost hold expires and schedtune
 * boost will expire immediately the task stops.
 * If disabled, this behaviour will only apply to tasks of the
 * RT class.
 */
#define SCHED_FEAT_SCHEDTUNE_BOOST_HOLD_ALL 0

/*
 * EEVDF: preserve lag when placing entities.
 *
 * Enabled by default through sysctl_sched_place_lag.
 * It is only useful together with the classic wakeup preemption rule
 * (sched_wakeup_preempt_gran != 0): with the EEVDF pick based rule a
 * preserved negative lag makes a freshly woken task ineligible and it loses
 * wakeup preemption entirely.
 */
#define SCHED_FEAT_PLACE_LAG READ_ONCE(sysctl_sched_place_lag)

/*
 * EEVDF: start new tasks with half a slice.
 */
#define SCHED_FEAT_PLACE_DEADLINE_INITIAL 1

/*
 * EEVDF: once selected, keep running a task until it runs to its
 * deadline (or becomes ineligible).
 */
#define SCHED_FEAT_RUN_TO_PARITY 1

/*
 * EEVDF: allow a wakeup of a task with a shorter slice to cancel the
 * RUN_TO_PARITY protection of the current task.
 */
#define SCHED_FEAT_PREEMPT_SHORT 1

/*
 * EEVDF: preserve the relative virtual deadline across 'migration'.
 */
#define SCHED_FEAT_PLACE_REL_DEADLINE 1

/*
 * Allow pick_eevdf() to select the ->next buddy, this affects latency
 * but not fairness.
 */
#define SCHED_FEAT_PICK_BUDDY 1
